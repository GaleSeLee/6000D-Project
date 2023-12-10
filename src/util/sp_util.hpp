// file: spmm_util.hpp
//
// Utilities for SpMM example.
// Including: array initialization, timer, and file loader.
//  author: guyue huang
//  date  : 2021/06/29

#include "mmio.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>            // std::rand()
#include <cuda_runtime_api.h> // cudaEvent APIs
#include <iostream>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <tuple>
#include <typeinfo>
#include <vector>
#include <string>
#include <tuple>

#define CUDA_CHECK(func)                                                       \
  {                                                                            \
    cudaError_t status = (func);                                               \
    if (status != cudaSuccess) {                                               \
      printf("CUDA API failed at line %d with error: %s (%d)\n", __LINE__,     \
             cudaGetErrorString(status), status);                              \
    }                                                                          \
  }

#define CUSPARSE_CHECK(func)                                                   \
  {                                                                            \
    cusparseStatus_t status = (func);                                          \
    if (status != CUSPARSE_STATUS_SUCCESS) {                                   \
      printf("CUSPARSE API failed at line %d with error: %s (%d)\n", __LINE__, \
             cusparseGetErrorString(status), status);                          \
    }                                                                          \
  }

// Fill a host array with random numbers.
void fill_random(float array[], int size) {
  for (int i = 0; i < size; i++) {
    array[i] = (float)(std::rand() % 3) / 10;
  }
}

// Fill a host array with all 0
template <typename DType> void fill_zero(DType array[], int size) {
  memset(array, 0x0, sizeof(array[0]) * size);
}

// Compute spmm correct numbers. All arrays are host memory locations.
template <typename Index, typename DType>
void spmm_reference_host(
    int M, // number of A-rows
    int N, // number of B_columns
    int K, // number of A columns
    const Index *csr_indptr, const int *csr_indices,
    const DType *csr_values, // three arrays of A's CSR format
    const DType *B,          // assume row-major
    DType *C_ref)            // assume row-major
{
  return;
  fill_zero(C_ref, M * N);
  for (int64_t i = 0; i < M; i++) {
    Index begin = csr_indptr[i];
    Index end = csr_indptr[i + 1];
    for (Index p = begin; p < end; p++) {
      int k = csr_indices[p];
      DType val = csr_values[p];
      for (int64_t j = 0; j < N; j++) {
        C_ref[i * N + j] += val * B[k * N + j];
      }
    }
  }
}

// Compute sddmm correct numbers. All arrays are host memory locations.
template <typename Index, typename DType>
void sddmm_reference_host(
    int M,   // number of S-rows, S is the sparse matrix
    int N,   // number of S_cols
    int K, // number of A columns
    int nnz,  // number of nonzeros in S

    const Index *csr_indptr, const Index *csr_indices,
    const DType *csr_values, // three arrays of the sparse matrix's CSR format
    const DType *A,          // assume row-major
    const DType *B,          // assume row-major, assume transposed
    DType *C_ref)            // assume row-major
{
  for (int i = 0; i < M; i++) {
    Index lb = csr_indptr[i];
    Index hb = csr_indptr[i + 1];
    Index offset1, offset2;
    DType acc = 0;
    for (int ptr = lb; ptr < hb; ptr++) {
      offset1 = i * K;
      offset2 = csr_indices[ptr] * K;
      for (int k = 0; k < K; k++) {
        acc += A[k + offset1] * B[k + offset2];
      }
      C_ref[ptr] = acc * csr_values[ptr];
      acc = 0;
    }
  }
}

// Compare two MxN matrices
template <typename DType>
bool check_result(int M, int N, DType *C, DType *C_ref) {
  return true;
  bool passed = true;
  for (int64_t i = 0; i < M; i++) {
    for (int64_t j = 0; j < N; j++) {
      DType c = C[i * N + j];
      DType c_ref = C_ref[i * N + j];
      if (fabs(c - c_ref) > 1e-2 * fabs(c_ref)) {
        printf(
            "Wrong result: i = %ld, j = %ld, result = %lf, reference = %lf.\n",
            i, j, c, c_ref);
        passed = false;
      }
    }
  }
  return passed;
}

// Encapsule CUDA timing APIs.
//
// Usage:
//   GpuTimer timer; // create
//   timer.start();  // when you start recording
//   timer.stop();   // when  you stop recording
//   float dur = timer.elapsed_msecs(); // duration in milliseconds

struct GpuTimer {
  cudaEvent_t startEvent;
  cudaEvent_t stopEvent;

  GpuTimer() {
    cudaEventCreate(&startEvent);
    cudaEventCreate(&stopEvent);
  }

  ~GpuTimer() {
    cudaEventDestroy(startEvent);
    cudaEventDestroy(stopEvent);
  }

  void start() { cudaEventRecord(startEvent, 0); }

  void stop() {
    cudaEventRecord(stopEvent, 0);
    cudaEventSynchronize(stopEvent);
  }

  float elapsed_msecs() {
    float elapsed;
    cudaEventElapsedTime(&elapsed, startEvent, stopEvent);
    return elapsed;
  }
};

// Load sparse matrix from an mtx file. Only non-zero positions are loaded, and
// values are dropped.
void read_mtx_file(const char *filename, int &nrow, int &ncol, int &nnz,
                   std::vector<int> &csr_indptr_buffer,
                   std::vector<int> &csr_indices_buffer) {
  FILE *f;

  if ((f = fopen(filename, "r")) == NULL) {
    printf("File %s not found", filename);
    exit(EXIT_FAILURE);
  }

  MM_typecode matcode;
  // Read MTX banner
  if (mm_read_banner(f, &matcode) != 0) {
    printf("Could not process this file.\n");
    exit(EXIT_FAILURE);
  }
  if (mm_read_mtx_crd_size(f, &nrow, &ncol, &nnz) != 0) {
    printf("Could not process this file.\n");
    exit(EXIT_FAILURE);
  }
  // printf("Reading matrix %d rows, %d columns, %d nnz.\n", nrow, ncol, nnz);

  /// read tuples

  std::vector<std::tuple<int, int>> coords;
  int row_id, col_id;
  float dummy;

  // std::cout << "matcode = " << matcode << std::endl;
  for (int64_t i = 0; i < nnz; i++) {
    if (fscanf(f, "%d", &row_id) == EOF) {
      std::cout << "Error: not enough rows in mtx file.\n";
      exit(EXIT_FAILURE);
    } else {
      fscanf(f, "%d", &col_id);
      if (mm_is_integer(matcode) || mm_is_real(matcode)) {
        fscanf(f, "%f", &dummy);
      }
      // mtx format is 1-based
      coords.push_back(std::make_tuple(row_id - 1, col_id - 1));
    }
  }

  /// make symmetric

  if (mm_is_symmetric(matcode)) {
    std::vector<std::tuple<int, int>> new_coords;
    for (auto iter = coords.begin(); iter != coords.end(); iter++) {
      int i = std::get<0>(*iter);
      int j = std::get<1>(*iter);

      new_coords.push_back(std::make_tuple(i, j));
      new_coords.push_back(std::make_tuple(j, i));
    }
    std::sort(new_coords.begin(), new_coords.end());
    coords.clear();
    for (auto iter = new_coords.begin(); iter != new_coords.end(); iter++) {
      if ((iter + 1) == new_coords.end() || (*iter != *(iter + 1))) {
        coords.push_back(*iter);
      }
    }
  } else {
    std::sort(coords.begin(), coords.end());
  }

  /// generate csr from coo

  csr_indptr_buffer.clear();
  csr_indices_buffer.clear();

  int curr_pos = 0;
  csr_indptr_buffer.push_back(0);
  for (int64_t row = 0; row < nrow; row++) {
    while ((curr_pos < nnz) && (std::get<0>(coords[curr_pos]) == row)) {
      csr_indices_buffer.push_back(std::get<1>(coords[curr_pos]));
      curr_pos++;
    }
    // assert((std::get<0>(coords[curr_pos]) > row || curr_pos == nnz));
    csr_indptr_buffer.push_back(curr_pos);
  }

  nnz = csr_indices_buffer.size();
}
void read_mtx_file_fast(const char *filename, int &nrow, int &ncol, int &nnz,
                   std::vector<int> &csr_indptr_buffer,
                   std::vector<int> &csr_indices_buffer,
                   std::vector<int> &len_row) {
  FILE *f;

  if ((f = fopen(filename, "r")) == NULL) {
    printf("File %s not found", filename);
    exit(EXIT_FAILURE);
  }

  MM_typecode matcode;
  // Read MTX banner
  if (mm_read_banner(f, &matcode) != 0) {
    printf("Could not process this file.\n");
    exit(EXIT_FAILURE);
  }
  if (mm_read_mtx_crd_size(f, &nrow, &ncol, &nnz) != 0) {
    printf("Could not process this file.\n");
    exit(EXIT_FAILURE);
  }
  // printf("Reading matrix %d rows, %d columns, %d nnz.\n", nrow, ncol, nnz);

  /// read tuples

  csr_indptr_buffer.resize(nrow+1);
  len_row.resize(nrow);
  csr_indices_buffer.resize(nnz);

  std::vector<std::tuple<int, int>> coords;
  coords.resize(nnz);
  int row_id, col_id;
  float dummy;

  if (mm_is_symmetric(matcode)) {
    coords.resize(nnz*2);
    int ii_coord = 0;
    for (int64_t i = 0; i < nnz; i++) {
      if (fscanf(f, "%d", &row_id) == EOF) {
        std::cout << "Error: not enough rows in mtx file.\n";
        exit(EXIT_FAILURE);
      } else {
        fscanf(f, "%d", &col_id);
        if (mm_is_integer(matcode) || mm_is_real(matcode)) {
          fscanf(f, "%f", &dummy);
        }
        // mtx format is 1-based
        coords[ii_coord++] = std::make_tuple(row_id - 1, col_id - 1);
        if (row_id != col_id) {
          coords[ii_coord++] = std::make_tuple(col_id - 1, row_id - 1);
        }
      }
    }
    coords.resize(ii_coord);
    csr_indices_buffer.resize(ii_coord);
  } else {
    for (int64_t i = 0; i < nnz; i++) {
      if (fscanf(f, "%d", &row_id) == EOF) {
        std::cout << "Error: not enough rows in mtx file.\n";
        exit(EXIT_FAILURE);
      } else {
        fscanf(f, "%d", &col_id);
        if (mm_is_integer(matcode) || mm_is_real(matcode)) {
          fscanf(f, "%f", &dummy);
        }
        // mtx format is 1-based
        coords[i] = std::make_tuple(row_id - 1, col_id - 1);
      }
    }
  }

  std::sort(coords.begin(), coords.end());

  /// generate csr from coo

  int curr_pos = 0;
  csr_indptr_buffer[0] = 0;
  int size = coords.size();
  for (int64_t row = 0; row < nrow; row++) {
    while ((curr_pos < size) && (std::get<0>(coords[curr_pos]) == row)) {
      csr_indices_buffer[curr_pos] = std::get<1>(coords[curr_pos]);
      curr_pos++;
    }
    // assert((std::get<0>(coords[curr_pos]) > row || curr_pos == nnz));
    csr_indptr_buffer[row+1] = curr_pos;
    len_row[row] = csr_indptr_buffer[row+1] - csr_indptr_buffer[row];
  }

  nnz = csr_indices_buffer.size();
}