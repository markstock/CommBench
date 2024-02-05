import pyComm
import numpy as np

#pyComm.init()
#pyComm.setup_gpu()
#pyComm.Comm.setprintid(0)
sendbuf = pyComm.pyalloc(1024)
tempbuf = pyComm.pyalloc(1024)
recvbuf = pyComm.pyalloc(1024)
c = pyComm.Comm(pyComm.library.IPC)
c1 = pyComm.Comm(pyComm.library.MPI)
c2 = pyComm.Comm(pyComm.library.IPC)
#c.add_lazy(1024, 0, 1)
#c1.add_lazy(256, 0, 4)
#c1.add_lazy(256, 1, 5)
#c1.add_lazy(256, 2, 6)
#c1.add_lazy(256, 3, 7)
c.add(sendbuf, 256, tempbuf, 0, 256, 0, 1)
c.add(sendbuf, 512, tempbuf, 0, 256, 0, 2)
c.add(sendbuf, 768, tempbuf, 0, 256, 0, 3)
c1.add(tempbuf, 0, tempbuf, 0, 256, 0, 4)
c1.add(tempbuf, 0, tempbuf, 0, 256, 1, 5)
c1.add(tempbuf, 0, tempbuf, 0, 256, 2, 6)
c1.add(tempbuf, 0, tempbuf, 0, 256, 3, 7)
c2.add(tempbuf, 0, recvbuf, 256, 256, 5, 4)
c2.add(tempbuf, 0, recvbuf, 512, 256, 6, 4)
c2.add(tempbuf, 0, recvbuf, 768, 256, 7, 4)
c.measure(5, 10)
c1.measure(5, 10)
c2.measure(5,10)
#c.start()
#c.wait()
#c1.start()
#c1.wait()
#c2.start()
#c2.wait()
pyComm.pyalloc.free(sendbuf)
pyComm.pyalloc.free(tempbuf)
pyComm.pyalloc.free(recvbuf)
#
#Comm.fin()
