
# Live555ClientSource

## Build

- Linux
```bash
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=./install -DCMAKE_CXX_FLAGS=-fPIC -DNO_OPENSSL=1 -DLIVE555_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release ..
make
make install
```
- Windows
see <https://learn.microsoft.com/en-us/cpp/build/reference/nmake-reference?view=msvc-170>
```bash
mkdir build && cd build
cmake -G "NMake Makefiles" -DCMAKE_INSTALL_PREFIX=./install -DNO_OPENSSL=1 -DLIVE555_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release ..
make
make install
```

For documentation and instructions for building this software,
see <http://www.live555.com/liveMedia/>
