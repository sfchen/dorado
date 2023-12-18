#include "caller_creation.h"

#if DORADO_GPU_BUILD
#ifndef __APPLE__
#include "basecall/CudaCaller.h"
#else
#include "basecall/MetalCaller.h"
#endif
#endif

namespace dorado::callers {

#if DORADO_GPU_BUILD
#ifndef __APPLE__
std::shared_ptr<basecall::CudaCaller> create_cuda_caller(
        const basecall::CRFModelConfig &model_config,
        int chunk_size,
        int batch_size,
        const std::string &device,
        float memory_limit_fraction,
        bool exclusive_gpu_access) {
    return std::make_shared<basecall::CudaCaller>(model_config, chunk_size, batch_size, device,
                                                  memory_limit_fraction, exclusive_gpu_access);
}
#else
std::shared_ptr<basecall::MetalCaller>
create_metal_caller(const basecall::CRFModelConfig &model_config, int chunk_size, int batch_size) {
    return std::make_shared<basecall::MetalCaller>(model_config, chunk_size, batch_size);
}

#endif
#endif

}  // namespace dorado::callers