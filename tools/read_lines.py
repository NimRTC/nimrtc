f = open("/Users/wufengzhang/NimRTC/src/third_party/usrsctp/src/usrsctplib/user_recv_thread.c")
lines = f.readlines()
f.close()
for i in range(30, 60):
    print(i, lines[i], end='')
