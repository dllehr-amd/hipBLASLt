#include <gtest/gtest.h>
#include <algorithm>
#include <numeric>
#include <vector>
#include <cstdlib>
#include <hip/hip_runtime.h>
#include <hip/hip_runtime_api.h>
#include "../include/hipblaslt_random.hpp"
#include <hipblaslt/hipblaslt-ext-op.h>
#include <hipblaslt_init.hpp>

namespace {
    template<typename DType>
    void cpuSoftmax(DType *m, DType *a, std::uint32_t numRows, std::uint32_t numCols) {
        for (std::uint32_t i = 0; i < numRows; ++i) {
            const auto rowMax = *std::max_element(a + i * numCols, a + i * numCols + numCols);
            auto rowSum = 0.f;
            std::transform(a + i * numCols, a + i * numCols + numCols, m + i * numCols, [&rowSum, rowMax] (auto v) {
                const auto u = std::exp(v - rowMax);
                rowSum += u;
                return u;
            });

            std::transform(m + i * numCols, m + i * numCols + numCols, m + i * numCols, [rowSum] (auto v) {
                return v / rowSum;
            });
        }
    }

    template<typename DType>
    void cpuLayerNorm(DType *out, DType *mean, DType *invvar, DType *in, std::uint32_t batch, std::uint32_t length, DType eps=1e-05, DType* gamma=nullptr, DType* beta=nullptr)
    {
        // calculate mean
        for(int i=0; i<batch; i++) {
            int count = 0;
            DType* inC  = in  + i * length;
            DType* outC = out + i * length;

            for(int j=0; j<length; j++) {
                count = count + 1;
                float delta = inC[j] - mean[i];
                mean[i] = mean[i] + delta / count;
                float delta2 = inC[j] - mean[i];
                invvar[i] = invvar[i] + delta * delta2;
            }
            invvar[i] = 1 / std::sqrt((invvar[i] / length) + eps);

            // calculate invvar
            for(int j=0; j<length; j++) {
                outC[j] = (inC[j] - mean[i]) * invvar[i];

                if (gamma != nullptr)
                    outC[j] = outC[j] * gamma[j];

                if (beta != nullptr)
                    outC[j] = outC[j] + beta[j];
            }
        }
    }
}

class ExtOpSoftmaxTest : public testing::TestWithParam<uint32_t> {};
class ExtOpSoftmaxUnsupportedDatatypeTest : public testing::TestWithParam<hipblasltDatatype_t> {};

class ExtOpLayerNormTest : public testing::TestWithParam<uint32_t> {};
class ExtOpLayerNormUnsupportedDatatypeTest : public testing::TestWithParam<hipblasltDatatype_t> {};

TEST_P(ExtOpSoftmaxTest, softmaxSuccess) {
    uint32_t m = GetParam();
    uint32_t n = 16;
    std::vector<float> input(m * n, 0.f);
    std::vector<float> output(m * n, 0.f);
    hipblaslt_uniform_int_1_10_run_float(input.data(), input.size());
    float *gpuInput{};
    float *gpuOutput{};
    auto err = hipMalloc(&gpuInput, m * n * sizeof(float));
    err = hipMalloc(&gpuOutput, m * n * sizeof(float));
    err = hipMemcpyHtoD(gpuInput, input.data(), m * n * sizeof(float));
    auto hipblasltErr = hipblasltExtSoftmax(HIPBLASLT_R_32F, m, n, 1, gpuOutput, gpuInput, nullptr);
    EXPECT_EQ(hipblasltErr, HIPBLAS_STATUS_SUCCESS);
    err = hipDeviceSynchronize();
    EXPECT_EQ(err, hipSuccess);
    std::vector<float> cpuRef(m * n, 0.f);
    cpuSoftmax(cpuRef.data(), input.data(), m, n);
    err = hipMemcpyDtoH(output.data(), gpuOutput, m * n * sizeof(float));

    for (std::size_t i = 0; i < m * n; ++i) {
        EXPECT_NEAR(output[i], cpuRef[i], 1e-5);
    }

    err = hipFree(gpuInput);
    err = hipFree(gpuOutput);
}

TEST_P(ExtOpLayerNormTest, layernormSuccess) {
    uint32_t m = GetParam();
    uint32_t n = 16;

    std::vector<float> output(m * n, 0.f);
    std::vector<float> mean(m, 0.f);
    std::vector<float> invvar(m, 0.f);
    std::vector<float> input(m * n, 0.f);
    std::vector<float> gamma(n, 1.f);
    std::vector<float> beta(n, 0.f);

    hipblaslt_init_hpl(input, n, m, n);
    hipblaslt_init_hpl(gamma, n, 1, n);
    hipblaslt_init_hpl(beta, n, 1, n);

    float *gpuOutput{};
    float *gpuMean{};
    float *gpuInvvar{};
    float *gpuInput{};
    float *gpuGamma{};
    float *gpuBeta{};

    auto err = hipMalloc(&gpuOutput, m * n * sizeof(float));
    err = hipMalloc(&gpuMean, m * sizeof(float));
    err = hipMalloc(&gpuInvvar, m * sizeof(float));
    err = hipMalloc(&gpuInput, m * n * sizeof(float));
    err = hipMalloc(&gpuGamma, n * sizeof(float));
    err = hipMalloc(&gpuBeta, n * sizeof(float));

    err = hipMemcpyHtoD(gpuInput, input.data(), m * n * sizeof(float));
    err = hipMemcpyHtoD(gpuGamma, gamma.data(), n * sizeof(float));
    err = hipMemcpyHtoD(gpuBeta,  beta.data(),  n * sizeof(float));

    auto hipblasltErr = hipblasltExtLayerNorm(HIPBLASLT_R_32F, gpuOutput, gpuMean, gpuInvvar, gpuInput, m, n, 1e-05, gpuGamma, gpuBeta, nullptr);
    EXPECT_EQ(hipblasltErr, HIPBLAS_STATUS_SUCCESS);
    err = hipDeviceSynchronize();
    EXPECT_EQ(err, hipSuccess);

    std::vector<float> cpuRef(m * n, 0.0f);
    std::vector<float> cpuMean(m, 0.0f);
    std::vector<float> cpuInvvar(m, 0.0f);
    cpuLayerNorm<float>(cpuRef.data(), cpuMean.data(), cpuInvvar.data(), input.data(), m, n, 1e-05, gamma.data(), beta.data());

    err = hipMemcpyDtoH(output.data(), gpuOutput, m * n * sizeof(float));
    err = hipMemcpyDtoH(mean.data(), gpuMean, m * sizeof(float));
    err = hipMemcpyDtoH(invvar.data(), gpuInvvar, m * sizeof(float));

    for (std::size_t i = 0; i < m * n; ++i) {
        EXPECT_NEAR(output[i], cpuRef[i], 1e-5);
    }
    for (std::size_t i = 0; i < m; ++i) {
        EXPECT_NEAR(mean[i], cpuMean[i], 1e-5);
    }
    for (std::size_t i = 0; i < m; ++i) {
        EXPECT_NEAR(invvar[i], cpuInvvar[i], 1e-5);
    }

    err = hipFree(gpuOutput);
    err = hipFree(gpuMean);
    err = hipFree(gpuInvvar);
    err = hipFree(gpuInput);
}

TEST_P(ExtOpSoftmaxUnsupportedDatatypeTest, softmaxFailureUnsupportedDatatype) {
    auto hipblasltErr = hipblasltExtSoftmax(GetParam(), 16, 16, 1, nullptr, nullptr, nullptr);
    EXPECT_EQ(hipblasltErr, HIPBLAS_STATUS_NOT_SUPPORTED);
}

TEST(ExtOpTest, softmaxFailureUnsupportedShapeOrReductionDim) {
    auto hipblasltErr = hipblasltExtSoftmax(HIPBLASLT_R_32F, 16, 512, 1, nullptr, nullptr, nullptr);
    EXPECT_EQ(hipblasltErr, HIPBLAS_STATUS_INVALID_VALUE);
    hipblasltErr = hipblasltExtSoftmax(HIPBLASLT_R_32F, 16, 16, 0, nullptr, nullptr, nullptr);
    EXPECT_EQ(hipblasltErr, HIPBLAS_STATUS_NOT_SUPPORTED);
}

TEST_P(ExtOpLayerNormUnsupportedDatatypeTest, layernormFailureUnsupportedDatatype) {
    auto hipblasltErr = hipblasltExtLayerNorm(GetParam(), nullptr, nullptr, nullptr, nullptr, 16, 1024, 1e-05, nullptr, nullptr, nullptr);
    EXPECT_EQ(hipblasltErr, HIPBLAS_STATUS_NOT_SUPPORTED);
}

TEST(ExtOpTest, layernormFailureInvalidValue) {
    auto hipblasltErr = hipblasltExtLayerNorm(HIPBLASLT_R_32F, nullptr, nullptr, nullptr, nullptr, 16, 1024, 1e-05, nullptr, nullptr, nullptr);
    EXPECT_EQ(hipblasltErr, HIPBLAS_STATUS_INVALID_VALUE);
}

INSTANTIATE_TEST_SUITE_P(ExtOpTest, ExtOpSoftmaxTest, testing::Values<uint32_t>(1, 16, 1335));
INSTANTIATE_TEST_SUITE_P(ExtOpTest, ExtOpSoftmaxUnsupportedDatatypeTest, testing::Values<hipblasltDatatype_t>(HIPBLASLT_R_16F, HIPBLASLT_R_16B));

INSTANTIATE_TEST_SUITE_P(ExtOpTest, ExtOpLayerNormTest, testing::Values<uint32_t>(1, 16, 1335, 6666));
INSTANTIATE_TEST_SUITE_P(ExtOpTest, ExtOpLayerNormUnsupportedDatatypeTest, testing::Values<hipblasltDatatype_t>(HIPBLASLT_R_16F, HIPBLASLT_R_16B));
