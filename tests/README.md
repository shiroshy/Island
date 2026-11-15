Проверки собираются LLVM/MinGW из комплекта сборки:

```powershell
./tests/regressions.ps1 -CompilerRoot ./.build/llvm-mingw-20261020-ucrt-x86_64
./tests/wallpaper_engine.ps1 -CompilerRoot ./.build/llvm-mingw-20261020-ucrt-x86_64 -OutputRoot ./.build/engine-regression
```

Первая команда проверяет обработку MP4 и размещение значков в отдельном невидимом Win32 desktop. Переключения рабочего стола не происходит.

Вторая использует собственный имитатор Wallpaper Engine, два окна обоев и захват тестового окна для проверки композиции. Нужны доступный Direct3D и Windows Graphics Capture. Захват используется только тестом. Тест не запускает установленный Wallpaper Engine и не меняет его обои.
