# Pro 3 Exchange Tests

These tests use synthetic MIDI and mocked REAPER calls. They do not connect to hardware. The C++ suite requires a C++11 compiler, CMake, Git, `pkg-config`, and FreeType/fontconfig development libraries. The Lua suite requires Lua 5.3 or later.

Build the [ysfx](https://github.com/jpcima/ysfx) test library if it is not already available:

```bash
git clone --depth 1 https://github.com/jpcima/ysfx.git "$HOME/.cache/pro3-exchange-tests/ysfx"
git -C "$HOME/.cache/pro3-exchange-tests/ysfx" submodule update --init thirdparty/dr_libs
cmake -S "$HOME/.cache/pro3-exchange-tests/ysfx" -B "$HOME/.cache/pro3-exchange-tests/ysfx/build" -DYSFX_PLUGIN=OFF -DYSFX_GFX=ON -DYSFX_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$HOME/.cache/pro3-exchange-tests/ysfx/build" -j2
```

From the repository root, build and run the Pro 3 JSFX suite:

```bash
c++ -std=c++11 -I "$HOME/.cache/pro3-exchange-tests/ysfx/include" tests/pro3_connection_check_test.cpp "$HOME/.cache/pro3-exchange-tests/ysfx/build/libysfx.a" $(pkg-config --libs freetype2 fontconfig) -lpthread -ldl -o "$HOME/.cache/pro3-exchange-tests/pro3_test"
"$HOME/.cache/pro3-exchange-tests/pro3_test" "$PWD/Effects/Pro 3 Exchange/Pro 3 Exchange.jsfx" "$PWD/tests/pro3_sender_test_controller.jsfx"
```

The Lua test loads the helper relative to its working directory, so run it from `Scripts/`:

```bash
cd Scripts
lua ../tests/pro3_patch_sender_test.lua
```
