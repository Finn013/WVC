#pragma once

#include <memory>
#include <vector>
#include <baseclasses/streams.h>
#include "FrameBuffer.h"


namespace softcam {


// WVC 0.14: one video format the camera offers (RGB24)
struct VideoFormat
{
    int     width;
    int     height;
    float   framerate;
};

class SoftcamStream;

class Softcam : public CSource, public IAMStreamConfig
{
public:
    static CUnknown* CreateInstance(
                    LPUNKNOWN   lpunk,
                    const GUID& clsid,
                    HRESULT*    phr);

    // IUnknown Methods
    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv) override;

    // IAMStreamConfig
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE *mt) override;
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE **out_pmt) override;
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int *out_count, int *out_size) override;
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc) override;

    FrameBuffer*    getFrameBuffer();
    void            releaseFrameBuffer();

    // WVC 0.14: a fixed list of formats that does not depend on whether WVC is running
    const std::vector<VideoFormat>& formats() const { return m_formats; }
    VideoFormat     defaultFormat() const { return m_formats.front(); }
    VideoFormat     preferredFormat();          // the one set by SetFormat, else the default
    bool            setFormat(VideoFormat* out); // true when SetFormat chose one

private:
    CCritSec    m_critsec;
    FrameBuffer m_frame_buffer;
    unsigned long m_last_open_try = 0;  // WVC: GetTickCount of the last attempt to find the sender

    CCritSec    m_fmt_lock;
    std::vector<VideoFormat> m_formats;
    bool        m_has_set = false;
    VideoFormat m_set{};

    SoftcamStream*  stream();

    Softcam(LPUNKNOWN lpunk, const GUID& clsid, HRESULT *phr);
};


class SoftcamStream : public CSourceStream, public IKsPropertySet, public IAMStreamConfig
{
 public:
    SoftcamStream(HRESULT *phr, Softcam *pParent, LPCWSTR pPinName);
    ~SoftcamStream();

    // IUnknown
    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv) override;

    // CBasePin
    STDMETHODIMP Notify(IBaseFilter * pSender, Quality q) override;
    HRESULT SetMediaType(const CMediaType *pmt) override;

    // CBaseOutputPin
    // Ask for buffers of the size appropriate to the agreed media type
    HRESULT DecideBufferSize(IMemAllocator *pIMemAlloc,
                             ALLOCATOR_PROPERTIES *pProperties) override;

    // CSourceStream
    HRESULT FillBuffer(IMediaSample *pms) override;
    HRESULT GetMediaType(CMediaType *pMediaType) override;
    HRESULT GetMediaType(int iPosition, CMediaType *pMediaType) override;
    HRESULT CheckMediaType(const CMediaType *pMediaType) override;
    HRESULT OnThreadCreate(void) override;
    HRESULT OnThreadDestroy(void) override;

    //  IKsPropertySet
    HRESULT STDMETHODCALLTYPE Set(REFGUID guidPropSet, DWORD dwPropID,
                                  LPVOID pInstanceData, DWORD cbInstanceData,
                                  LPVOID pPropData, DWORD cbPropData) override;
    HRESULT STDMETHODCALLTYPE Get(REFGUID guidPropSet, DWORD dwPropID,
                                  LPVOID pInstanceData, DWORD cbInstanceData,
                                  LPVOID pPropData, DWORD cbPropData, DWORD *pcbReturned) override;
    HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID guidPropSet, DWORD dwPropID,
                                             DWORD *pTypeSupport) override;

    // IAMStreamConfig
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE *mt) override;
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE **out_pmt) override;
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int *out_count, int *out_size) override;
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc) override;

    // WVC 0.14: after SetFormat — reconnect with the new size if the pin is already connected
    void    reconnectFor(const VideoFormat& f);
    HRESULT copyConnectedType(AM_MEDIA_TYPE **out_pmt);

private:
    uint64_t    m_frame_counter = 0;
    std::unique_ptr<uint8_t[]>  m_screenshot;   // WVC: the placeholder frame (DIB rw x rh), built when first needed
    int         m_ph_w = 0;
    int         m_ph_h = 0;
    bool        m_placeholder_ready = false;
    void        buildPlaceholder(int w, int h);
    std::vector<uint8_t> m_scratch;             // when the sample is smaller than the frame

    CCritSec m_critsec;
    LONGLONG m_sample_time = 0;                  // 100 ns
    long m_interval_time_msec = 33;
    Timer   m_run_timer;                         // since the first sample of this run
    Timer   m_pace_timer;                        // since the last delivered sample
    bool    m_first_sample = true;

    Softcam*        getParent();
};


} //namespace softcam
