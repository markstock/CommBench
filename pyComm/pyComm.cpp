#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
//  #include "../comm.h"

namespace py = pybind11;

#include <mpi.h>
#include <stdio.h> // for printf
#include <string.h> // for memcpy
#include <algorithm> // for std::sort
#include <vector> // for std::vector

namespace CommBench {
    static int printid = 0;
    enum library {null, MPI, NCCL, IPC, STAGE, numlib};
    static MPI_Comm comm_mpi;

    void mpi_init();
    void mpi_fin();

    template <typename T>
    class Comm {
        public:
            const library lib;
            Comm(library lib);
            void add_lazy(size_t count, int sendid, int recvid);
            void measure(int warmup, int numiter, double &minTime, double &medTime, double &avgTime, double &maxTime);
            void measure(int warmup, int numiter);
            void measure(int warmup, int numiter, size_t data);
            void measure_count(int warmup, int numiter, size_t data);
    };
};

void CommBench::mpi_init() {
    MPI_Init(NULL, NULL);
}

void CommBench::mpi_fin() {
    MPI_Finalize();
}

template <typename T>
void CommBench::Comm<T>::add_lazy(size_t count, int sendid, int recvid) {
    T *sendbuf;
    T *recvbuf;
    allocate(sendbuf, count, sendid);
    allocate(recvbuf, count, recvid);
    add(sendbuf, 0, recvbuf, 0, count, sendid, recvid);
}

template <typename T>
void Comm<T>::measure(int warmup, int numiter) {
    measure(warmup, numiter, 0);
}

template <typename T>
void Comm<T>::measure(int warmup, int numiter, size_t count) {
    if(count == 0) {
      long count_total = 0;
      for(int send = 0; send < numsend; send++)
         count_total += sendcount[send];
      MPI_Allreduce(MPI_IN_PLACE, &count_total, 1, MPI_LONG, MPI_SUM, comm_mpi);
      measure_count(warmup, numiter, count_total);
    }
    else
      measure_count(warmup, numiter, count);
}

template <typename T>
void Comm<T>::measure_count(int warmup, int numiter, size_t count) {

    int myid;
    MPI_Comm_rank(comm_mpi, &myid);

    this->report();

    double minTime;
    double medTime;
    double maxTime;
    double avgTime;
    this->measure(warmup, numiter, minTime, medTime, maxTime, avgTime);

    if(myid == printid) {
      size_t data = count * sizeof(T);
      printf("data: "); print_data(data); printf("\n");
      printf("minTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", minTime * 1e6, minTime / data * 1e12, data / minTime / 1e9);
      printf("medTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", medTime * 1e6, medTime / data * 1e12, data / medTime / 1e9);
      printf("maxTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", maxTime * 1e6, maxTime / data * 1e12, data / maxTime / 1e9);
      printf("avgTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", avgTime * 1e6, avgTime / data * 1e12, data / avgTime / 1e9);
      printf("\n");
    }
};

template <typename T>
void Comm<T>::measure(int warmup, int numiter, double &minTime, double &medTime, double &maxTime, double &avgTime) {

    double times[numiter];
    double starts[numiter];
    int myid;
    MPI_Comm_rank(comm_mpi, &myid);

    if(myid == printid)
      printf("%d warmup iterations (in order):\n", warmup);
    for (int iter = -warmup; iter < numiter; iter++) {
      for(int send = 0; send < numsend; send++) {
#if defined PORT_CUDA
        // cudaMemset(sendbuf[send], -1, sendcount[send] * sizeof(T));
#elif defined PORT_HIP
        // hipMemset(sendbuf[send], -1, sendcount[send] * sizeof(T));
#elif defined PORT_SYCL
	// q->memset(sendbuf[send], -1, sendcount[send] * sizeof(T)).wait();
#else
        memset(sendbuf[send], -1, sendcount[send] * sizeof(T)); // NECESSARY FOR CPU TO PREVENT CACHING
#endif
      }
      MPI_Barrier(comm_mpi);
      double time = MPI_Wtime();
      this->start();
      double start = MPI_Wtime() - time;
      this->wait();
      time = MPI_Wtime() - time;
      MPI_Allreduce(MPI_IN_PLACE, &start, 1, MPI_DOUBLE, MPI_MAX, comm_mpi);
      MPI_Allreduce(MPI_IN_PLACE, &time, 1, MPI_DOUBLE, MPI_MAX, comm_mpi);
      if(iter < 0) {
        if(myid == printid)
          printf("startup %.2e warmup: %.2e\n", start * 1e6, time * 1e6);
      }
      else {
        starts[iter] = start;
        times[iter] = time;
      }
    }
    std::sort(times, times + numiter,  [](const double & a, const double & b) -> bool {return a < b;});
    std::sort(starts, starts + numiter,  [](const double & a, const double & b) -> bool {return a < b;});

    if(myid == printid) {
      printf("%d measurement iterations (sorted):\n", numiter);
      for(int iter = 0; iter < numiter; iter++) {
        printf("start: %.4e time: %.4e", starts[iter] * 1e6, times[iter] * 1e6);
        if(iter == 0)
          printf(" -> min\n");
        else if(iter == numiter / 2)
          printf(" -> median\n");
        else if(iter == numiter - 1)
          printf(" -> max\n");
        else
          printf("\n");
      }
      printf("\n");
    }

    minTime = times[0];
    medTime = times[numiter / 2];
    maxTime = times[numiter - 1];
    avgTime = 0;
    for(int iter = 0; iter < numiter; iter++)
      avgTime += times[iter];
    avgTime /= numiter;
}

template <typename T>
CommBench::Comm<T>::Comm(CommBench::library lib) : lib(lib) {
    int flag;
    MPI_Initialized(&flag);
    if(flag) {
        MPI_Comm_dup(MPI_COMM_WORLD, &CommBench::comm_mpi);
    } else {
        return;
    }  
    int myid;
    int numproc;
    MPI_Comm_rank(CommBench::comm_mpi, &myid);
    MPI_Comm_size(CommBench::comm_mpi, &numproc);
    if(myid == CommBench::printid) {
        printf("success.\n");
    }
};


PYBIND11_MODULE(pyComm, m) {
    py::enum_<CommBench::library>(m, "library")
        .value("null", CommBench::library::null)
        .value("MPI", CommBench::library::MPI)
        .value("NCCL", CommBench::library::NCCL)
        .value("IPC", CommBench::library::IPC)
        .value("STAGE", CommBench::library::STAGE)
        .value("numlib", CommBench::library::numlib);
    py::class_<CommBench::Comm<int>>(m, "Comm")
        .def(py::init<CommBench::library>())
        .def("mpi_init", &CommBench::mpi_init)
        .def("mpi_fin", &CommBench::mpi_fin)
        .def("add_lazy", &CommBench::add_lazy)
        // .def("measure", static_cast<void (CommBench::Comm::*)(int, int, double&, double&, double&, double&)>(&CommBench::Comm::measure), "measure the latency")
        .def("measure", static_cast<void (CommBench::Comm::*)(int, int)>(&CommBench::Comm::measure), "measure the latency")
        // .def("measure", static_cast<void (CommBench::Comm::*)(int, int, size_t)(&CommBench::Comm::measure), "measure the latency">)
        // .def("measure_count", static_cast<void (CommBench::Comm::*)(int, int, size_t)>(&CommBench::Comm::measure_count), "measure the latency");
        // .def("add", &CommBench::Comm<int>::add)
        // .def("start", &CommBench::Comm<int>::start)
        // .def("wait", &CommBench::Comm<int>::wait);
}
