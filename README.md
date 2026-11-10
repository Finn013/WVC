# WVC — Windows Virtual Camera (screen area)

Лёгкая виртуальная камера для Windows 10/11: показывает выбранную область экрана как веб-камеру «WVC Camera» (Zoom, Telegram, Discord, браузеры). Без OBS.

- Камера: DirectShow-фильтр на базе [tshino/softcam](https://github.com/tshino/softcam) (MIT), `third_party/softcam` (переименован: свой CLSID и имя).
- Приложение: `app/main.cpp` — Win32, выбор области, захват GDI, трей.
- Сборка: GitHub Actions (`.github/workflows/build.yml`) → релиз `latest` с `WVC.zip`.

Инструкция для пользователя: `docs/README-RU.txt`.
