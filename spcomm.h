
#include "comm.h"


namespace CommBench {

  template <typename T, typename I>
  struct sparse_t {
    // public:
    T *sendbuf;
    T *recvbuf;
    size_t count;
    size_t *offset;
    I *index;
    sparse_t(T *sendbuf, T *recvbuf, size_t count, size_t *offset, I *index) : sendbuf(sendbuf), recvbuf(recvbuf), count(count), offset(offset), index(index) {};
  };

#if defined PORT_CUDA || PORT_HIP
  template <typename T, typename I>
  __global__ void sparse_kernel(void *sparse_temp) {
    sparse_t<T,I> &sparse = *((sparse_t<T,I>*)sparse_temp);
    size_t tid = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    size_t count = sparse.count;
    if (tid < count) {
      T *sendbuf = sparse.sendbuf;
      T *recvbuf = sparse.recvbuf;
      size_t *offset = sparse.offset;
      I *index = sparse.index;
      if(offset == nullptr)
        recvbuf[tid] = sendbuf[index[tid]];
      else {
        T acc = 0;
        for (size_t i = offset[tid]; i < offset[tid + 1]; i++)
          acc += sendbuf[index[i]];
        recvbuf[tid] = acc;
      }
    }
  }
#elif defined SYCL
#else
  template <typename T, typename I>
  void sparse_kernel(void *sparse_temp) {
    sparse_t<T,I> &sparse = *((sparse_t<T,I>*)sparse_temp);
    T *sendbuf = sparse.sendbuf;
    T *recvbuf = sparse.recvbuf;
    size_t count = sparse.count;
    size_t *offset = sparse.offset;
    I *index = sparse.index;
    if(offset == nullptr) {
      #pragma parallel for
      for(size_t i = 0; i < count; i++)
        recvbuf[i] = sendbuf[index[i]];
    }
    else {
      #pragma parallel for
      for (size_t i = 0; i < count; i++) {
        T acc = 0;
        for (I j = offset[i]; j < offset[i + 1]; j++)
          acc += sendbuf[index[j]];
        recvbuf[i] = acc;
      }
    }
  }
#endif

  template <typename T, typename I>
  class SpComm : public Comm<T> {
    public:
    std::vector<void*> arg;
    std::vector<void (*)(void *)> func;
    std::vector<size_t> count;
    std::vector<int> precompid;
    std::vector<int> postcompid;

#ifdef PORT_CUDA
    std::vector<cudaStream_t> stream;
#elif defined PORT_HIP
    std::vector<hipStream_t> stream;
#elif defined PORT_SYCL
    std::vector<sycl::queue> queue;
#endif

    using Comm<T>::Comm;

    // MEMORY ALLOCATION FUNCTIONS
    T* allocate(size_t n, std::vector<int> vec) {
      T *buffer = nullptr;
      for (int i : vec)
        Comm<T>::allocate(buffer, n, i);
      return buffer;
    };
    T* allocate(size_t n, int i) {
      std::vector<int> v{i};
      return allocate(n, v);
    };
    T* allocate(size_t n) {
      std::vector<int> v;
      for (int i = 0; i < numproc; i++)
        v.push_back(i);
      return allocate(n, v);
    };

    // ADD COMPUTATION
    void add_comp(void func(void *), void *arg, size_t count) {
      this->count.push_back(count);
      this->arg.push_back(arg);
      this->func.push_back(func);
#ifdef PORT_CUDA
      stream.push_back(cudaStream_t());
      cudaStreamCreate(&stream[stream.size() - 1]);
#elif defined PORT_HIP
      stream.push_back(hipStream_t());
      hipStreamCreate(&stream[stream.size() - 1]);
#elif defined PORT_SYCL
      queue.push_back(sycl::queue(sycl::gpu_selector_v));
#endif
    }
    void add_precomp(void func(void *), void *arg, size_t count) {
      precompid.push_back(this->count.size());
      add_comp(func, arg, count);
    };
    void add_postcomp(void func(void *), void *arg, size_t count) {
      postcompid.push_back(this->count.size());
      add_comp(func, arg, count);
    };

    void add_gather(T *sendbuf, T *recvbuf, size_t count, I *index, int i) {
      if(myid == i) {
        MPI_Send(&sendbuf, sizeof(T*), MPI_BYTE, printid, 0, comm_mpi);
        MPI_Send(&recvbuf, sizeof(T*), MPI_BYTE, printid, 0, comm_mpi);
        MPI_Send(&count, sizeof(size_t), MPI_BYTE, printid, 0, comm_mpi);
        MPI_Send(&index, sizeof(I*), MPI_BYTE, printid, 0, comm_mpi);
      }
      if(myid == printid) {
        MPI_Recv(&sendbuf, sizeof(T*), MPI_BYTE, i, 0, comm_mpi, MPI_STATUS_IGNORE);
        MPI_Recv(&recvbuf, sizeof(T*), MPI_BYTE, i, 0, comm_mpi, MPI_STATUS_IGNORE);
        MPI_Recv(&count, sizeof(size_t), MPI_BYTE, i, 0, comm_mpi, MPI_STATUS_IGNORE);
        MPI_Recv(&index, sizeof(I*), MPI_BYTE, i, 0, comm_mpi, MPI_STATUS_IGNORE);
        if(count)
          printf("Bench %d proc %d add gather sendbuf %p recvbuf %p count %ld index %p\n", Comm<T>::benchid, myid, sendbuf, recvbuf, count, index);
      }
      if(count == 0) return;
      if(myid == i) {
        I *index_d;
        sparse_t<T, I> *sparse_d;
	CommBench::allocate(index_d, count);
	CommBench::allocate(sparse_d, 1);
        sparse_t<T, I> sparse(sendbuf, recvbuf, count, nullptr, index_d);
#ifdef PORT_CUDA
        cudaMemcpy(index_d, index, count * sizeof(I*), cudaMemcpyHostToDevice);
        cudaMemcpy(sparse_d, &sparse, sizeof(sparse_t<T, I>*), cudaMemcpyHostToDevice);
#elif defined PORT_HIP
        hipMemcpy(index_d, index, count * sizeof(I*), cudaMemcpyHostToDevice);
        hipMemcpy(sparse_d, &sparse, sizeof(sparse_t<T, I>*), cudaMemcpyHostToDevice);
#elif defined PORT_SYCL
        q.memcpy(index_d, index, count * sizeof(I*));
        q.memcpy(sparse_d, &sparse, sizeof(sparse_t<T, I>*));
#else
        memcpy(index_d, index, count * sizeof(I*));
        memcpy(sparse_d, &sparse, sizeof(sparse_t<T, I>*));
#endif
        add_precomp(sparse_kernel<T, I>, sparse_d, count);
      }
    }

    void start() {
      for (int i : precompid) {
#if defined PORT_CUDA || defined PORT_HIP
        const int blocksize = 256;
        func[i]<<<(count[i] + blocksize - 1) / blocksize, blocksize, 0, stream[i]>>>(arg[i]);
#elif defined PORT_SYCL
        queue.function(arg[i]);
#else
        sparse_kernel(arg[i]);
#endif
      }
      for (int i : precompid) {
#ifdef PORT_CUDA
        cudaStreamSynchronize(stream[i]);
#elif defined PORT_HIP
        hipStreamSynchronize(stream[i]);
#elif defined PORT_SYCL
        queue[i].wait();
#endif
      }
      Comm<T>::start();
    }

    void wait() {
      Comm<T>::wait();
      for (int i : postcompid) {
#if defined PORT_CUDA || defined PORT_HIP
        const int blocksize = 256;
        func[i]<<<(count[i] + blocksize - 1) / blocksize, blocksize, 0, stream[i]>>>(arg[i]);
#elif defined PORT_SYCL
        queue.function(arg[i]);
#else
        sparse_kernel(arg[i]);
#endif
      }
      for (int i : postcompid) {
#ifdef PORT_CUDA
        cudaStreamSynchronize(stream[i]);
#elif defined PORT_HIP
        hipStreamSynchronize(stream[i]);
#elif defined PORT_SYCL
        queue[i].wait();
#endif
      }
    }

  };
}
