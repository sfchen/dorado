#pragma once

namespace dorado::utils {

void make_torch_deterministic();
void set_torch_allocator_max_split_size(int size = 25);

}  // namespace dorado::utils
