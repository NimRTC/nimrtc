import pathlib
f = pathlib.Path("/Users/wufengzhang/NimRTC/src/third_party/usrsctp/CMakeLists.txt")
c = f.read_text()
old = "        INET6=1"
new = "        INET6=1\n        __APPLE_USE_RFC_3542=1"
c = c.replace(old, new, 1)
f.write_text(c)
print("done")
