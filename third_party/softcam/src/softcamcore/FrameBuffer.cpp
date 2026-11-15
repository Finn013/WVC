#include "FrameBuffer.h"

#include <windows.h>
#include <mutex> // lock_guard
#include <cstring>
#include <algorithm>
#include <vector>


namespace softcam {


const char NamedMutexName[] = "WVC Camera/NamedMutex";
const char SharedMemoryName[] = "WVC Camera/SharedMemory";
const uint8_t ProtocolVersion = 2;


struct FrameBuffer::Header
{
    uint32_t    m_image_offset;
    uint16_t    m_width;
    uint16_t    m_height;
    float       m_framerate;
    uint8_t     m_is_active;
    uint8_t     m_connected_min_version; // 0 or 1 or 2
    uint8_t     m_watchdog_sender_heartbeat;
    uint8_t     m_watchdog_receiver_heartbeat;
    uint64_t    m_frame_counter;

    uint8_t*    imageData();
};


uint8_t* FrameBuffer::Header::imageData()
{
    uint8_t *image = reinterpret_cast<uint8_t*>(this) + m_image_offset;
    return image;
}


FrameBuffer FrameBuffer::create(
                        int             width,
                        int             height,
                        float           framerate)
{
    FrameBuffer fb(NamedMutexName);

    if (!checkDimensions(width, height))
    {
        return fb;
    }
    if (framerate < 0.0f)
    {
        return fb;
    }

    // WVC 0.14: the block is made big enough for any WVC output (up to 1920x1920), so that a block still held by
    // Zoom/Telegram after WVC restarted its camera can be taken over at a new size instead of failing
    const uint32_t needed = calcMemorySize((uint16_t)width, (uint16_t)height);
    const uint32_t shmem_size = (std::max)(needed, calcMemorySize(1920, 1920));
    bool existed = false;
    fb.m_shmem = SharedMemory::createOrOpen(SharedMemoryName, shmem_size, &existed);
    if (fb.m_shmem && existed)
    {
        // somebody (a camera user, or a WVC that crashed) still holds the block
        bool ok = fb.m_shmem.size() >= needed && fb.m_shmem.size() >= sizeof(Header);
        if (ok)
        {
            uint8_t beat = 0, active = 0;
            {
                std::lock_guard<NamedMutex> lock(fb.m_mutex);
                active = fb.header()->m_is_active;
                beat = fb.header()->m_watchdog_sender_heartbeat;
            }
            if (active)
            {
                // still active: take it over only if its sender is dead (no heartbeat for 3 periods)
                Timer::sleep(WATCHDOG_HEARTBEAT_INTERVAL * 4);
                std::lock_guard<NamedMutex> lock(fb.m_mutex);
                ok = fb.header()->m_is_active == 0 || fb.header()->m_watchdog_sender_heartbeat == beat;
            }
        }
        if (!ok)
        {
            fb.m_shmem = {};
            return fb;
        }
    }
    if (fb.m_shmem)
    {
        std::lock_guard<NamedMutex> lock(fb.m_mutex);

        auto frame = fb.header();
        const uint64_t old_counter = existed ? frame->m_frame_counter : 0;
        frame->m_image_offset = sizeof(Header);
        frame->m_width = (uint16_t)width;
        frame->m_height = (uint16_t)height;
        frame->m_framerate = framerate;
        frame->m_is_active = 1;
        if (!existed)
        {
            frame->m_connected_min_version = 0;
            frame->m_watchdog_sender_heartbeat = 0;
            frame->m_watchdog_receiver_heartbeat = 0;
        }
        // keep counting up on a takeover, so a waiting receiver sees the next frame as new
        frame->m_frame_counter = old_counter;
        std::memset(frame->imageData(), 0, (std::size_t)3 * width * height);

        auto mutex = fb.m_mutex;
        fb.m_sender_watchdog = Watchdog::createHeartbeat(
            WATCHDOG_HEARTBEAT_INTERVAL,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                frame->m_watchdog_sender_heartbeat += 1;
            });
        fb.m_receiver_watchdog = Watchdog::createMonitor(
            WATCHDOG_MONITOR_INTERVAL,
            WATCHDOG_TIMEOUT,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                return frame->m_watchdog_receiver_heartbeat;
            });
    }
    return fb;
}

FrameBuffer FrameBuffer::none()
{
    return FrameBuffer(NamedMutexName);
}

FrameBuffer FrameBuffer::open()
{
    FrameBuffer fb(NamedMutexName);

    fb.m_shmem = SharedMemory::open(SharedMemoryName);
    if (fb.m_shmem)
    {
        std::lock_guard<NamedMutex> lock(fb.m_mutex);

        auto size = fb.m_shmem.size();
        if (size < sizeof(Header))
        {
            fb.m_shmem = {};
            return fb;
        }
        auto frame = fb.header();
        if (!checkDimensions(frame->m_width, frame->m_height) ||
            frame->m_framerate < 0.0f)
        {
            fb.m_shmem = {};
            return fb;
        }
        uint32_t image_size = (uint32_t)frame->m_width * (uint32_t)frame->m_height * 3;
        if (size <= frame->m_image_offset ||
            size - frame->m_image_offset < image_size)
        {
            fb.m_shmem = {};
            return fb;
        }

        auto mutex = fb.m_mutex;
        fb.m_sender_watchdog = Watchdog::createMonitor(
            WATCHDOG_MONITOR_INTERVAL,
            WATCHDOG_TIMEOUT,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                return frame->m_watchdog_sender_heartbeat;
            });
        fb.m_receiver_watchdog = Watchdog::createHeartbeat(
            WATCHDOG_HEARTBEAT_INTERVAL,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                frame->m_watchdog_receiver_heartbeat += 1;
            });
        if (0 == frame->m_connected_min_version ||
            ProtocolVersion <= frame->m_connected_min_version)
        {
            frame->m_connected_min_version = ProtocolVersion;
        }
        frame->m_watchdog_receiver_heartbeat += 1;
    }

    return fb;
}

FrameBuffer&
FrameBuffer::operator =(const FrameBuffer& fb)
{
    m_receiver_watchdog = {};
    m_sender_watchdog = {};
    m_shmem = {};
    m_shmem = fb.m_shmem;
    m_sender_watchdog = fb.m_sender_watchdog;
    m_receiver_watchdog = fb.m_receiver_watchdog;
    return *this;
}

void* FrameBuffer::handle() const
{
    return const_cast<void*>(m_shmem.get());
}

int FrameBuffer::width() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_width : 0;
}

int FrameBuffer::height() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_height : 0;
}

float FrameBuffer::framerate() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_framerate : 0.0f;
}

uint64_t FrameBuffer::frameCounter() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_frame_counter : 0;
}

bool FrameBuffer::active() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem && header()->m_is_active;
}

bool FrameBuffer::connected() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    if (m_shmem)
    {
        auto ver = header()->m_connected_min_version;
        if (0 == ver)
        {
            // No receivers connected
            return false;
        }
        if (1 == ver)
        {
            // At least one of the connected receivers is version 1.
            // Since receivers of version 1 don't have receiver-watchdog,
            // we won't know their disconnection.
            return true;
        }
        return m_receiver_watchdog.alive();
    }
    return false;
}

void FrameBuffer::deactivate()
{
    if (!m_shmem) return;
    std::lock_guard<NamedMutex> lock(m_mutex);
    header()->m_is_active = 0;
}

void FrameBuffer::write(const void* image_bits)
{
    if (!m_shmem) return;
    std::lock_guard<NamedMutex> lock(m_mutex);
    auto frame = header();
    std::memcpy(
            frame->imageData(),
            image_bits,
            (std::size_t)3 * frame->m_width * frame->m_height);
    frame->m_frame_counter += 1;
}

void FrameBuffer::transferToDIB(void* image_bits, uint64_t* out_frame_counter)
{
    if (!m_shmem)
    {
        *out_frame_counter = 0;
        return;
    }
    std::lock_guard<NamedMutex> lock(m_mutex);

    auto frame = header();
    {
        int w = frame->m_width;
        int h = frame->m_height;
        int gap = ((w * 3 + 3) & ~3) - w * 3;
        const std::uint8_t* image = frame->imageData();
        std::uint8_t* dest = (std::uint8_t*)image_bits;
        for (int y = 0; y < h; y++)
        {
            const std::uint8_t* src = image + 3 * w * (h - 1 - y);
            std::memcpy(dest, src, 3 * (uint32_t)w);
            dest += 3 * w + gap;
        }
        *out_frame_counter = frame->m_frame_counter;
    }
}

void FrameBuffer::transferToDIBScaled(void* image_bits, int dw, int dh, uint64_t* out_frame_counter)
{
    const int dstride = (dw * 3 + 3) & ~3;
    transferToDIBRect(image_bits, dstride, dh, 0, 0, dw, dh, out_frame_counter);
}

void FrameBuffer::transferToDIBRect(void* image_bits, int dstride, int rows,
                                    int rx, int ry, int rw, int rh, uint64_t* out_frame_counter)
{
    std::uint8_t* dest = (std::uint8_t*)image_bits;
    if (!dest || dstride <= 0 || rows <= 0 || rw <= 0 || rh <= 0 || rx < 0 || ry < 0 ||
        ry + rh > rows || (rx + rw) * 3 > dstride)
    {
        *out_frame_counter = 0;
        return;
    }
    auto rowPtr = [&](int y) { return dest + (std::size_t)dstride * (std::size_t)(rows - 1 - y); };  // y: top-down
    if (!m_shmem)
    {
        for (int y = ry; y < ry + rh; y++) std::memset(rowPtr(y) + (std::size_t)rx * 3, 0, (std::size_t)rw * 3);
        *out_frame_counter = 0;
        return;
    }
    std::lock_guard<NamedMutex> lock(m_mutex);
    auto frame = header();
    const int sw = frame->m_width, sh = frame->m_height;
    if (sw <= 0 || sh <= 0 || (uint64_t)m_shmem.size() < (uint64_t)frame->m_image_offset + 3ull * sw * sh)
    {
        for (int y = ry; y < ry + rh; y++) std::memset(rowPtr(y) + (std::size_t)rx * 3, 0, (std::size_t)rw * 3);
        *out_frame_counter = frame->m_frame_counter;
        return;
    }
    // fit, keeping proportions (black bars)
    int fw = rw, fh = (int)((long long)rw * sh / sw);
    if (fh > rh)
    {
        fh = rh;
        fw = (int)((long long)rh * sw / sh);
    }
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    if (fw > rw) fw = rw;
    if (fh > rh) fh = rh;
    const int ox = (rw - fw) / 2, oy = (rh - fh) / 2;
    const std::uint8_t* image = frame->imageData();
    thread_local std::vector<int> xs;
    xs.resize((std::size_t)fw);
    for (int x = 0; x < fw; x++) xs[(std::size_t)x] = 3 * (int)((long long)x * sw / fw);
    for (int y = 0; y < rh; y++)
    {
        std::uint8_t* d = rowPtr(ry + y) + (std::size_t)rx * 3;
        if (y < oy || y >= oy + fh)
        {
            std::memset(d, 0, (std::size_t)rw * 3);
            continue;
        }
        if (ox > 0) std::memset(d, 0, (std::size_t)ox * 3);
        if (rw - ox - fw > 0) std::memset(d + (std::size_t)(ox + fw) * 3, 0, (std::size_t)(rw - ox - fw) * 3);
        d += (std::size_t)ox * 3;
        const int sy = (int)((long long)(y - oy) * sh / fh);         // top-down source row
        const std::uint8_t* srow = image + (std::size_t)3 * sw * sy;
        if (fw == sw)
        {
            std::memcpy(d, srow, (std::size_t)3 * fw);
            continue;
        }
        for (int x = 0; x < fw; x++)
        {
            const std::uint8_t* s = srow + xs[(std::size_t)x];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d += 3;
        }
    }
    *out_frame_counter = frame->m_frame_counter;
}

bool FrameBuffer::waitForNewFrame(uint64_t frame_counter, float time_out)
{
    if (!m_shmem) return false;
    Timer timer;
    while (active() && m_sender_watchdog.alive())
    {
        if (frameCounter() > frame_counter)
        {
            return true;
        }
        Timer::sleep(0.001f);
        if (0.0f < time_out && time_out <= timer.get())
        {
            return true;
        }
    }
    return false;
}

void FrameBuffer::release()
{
    m_receiver_watchdog.stop();
    m_sender_watchdog.stop();
    m_shmem = SharedMemory{};
}

FrameBuffer::Header* FrameBuffer::header()
{
    return static_cast<Header*>(m_shmem.get());
}

const FrameBuffer::Header* FrameBuffer::header() const
{
    return static_cast<const Header*>(m_shmem.get());
}

bool FrameBuffer::checkDimensions(
                        int width,
                        int height)
{
    // We reject big numbers and numbers not multiple of four.
    // Too large pictures may exceed the 32-bit limit of DIBitmap and shared memory.
    // Sizes which are not multiple of four tend to cause problems in the subsequent
    // processes such as color conversion or compression in video applications.
    if (width < 1 || width > 16384 || width % 4 != 0 ||
        height < 1 || height > 16384 || height % 4 != 0)
    {
        return false;
    }
    return true;
}

uint32_t FrameBuffer::calcMemorySize(
                        uint16_t width,
                        uint16_t height)
{
    uint32_t header_size = sizeof(Header);
    uint32_t image_size = (uint32_t)width * height * 3;
    uint32_t shmem_size = header_size + image_size;
    return shmem_size;
}


} //namespace softcam
