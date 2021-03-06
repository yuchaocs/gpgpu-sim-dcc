//Jin: cuda_device_runtime.h
//Defines CUDA device runtime APIs for CDP support

//Po-Han: dynamic child-thread consolidation support

#pragma once

extern bool g_agg_blocks_support;

//Po-Han: dynamic child-thread consolidation support
class dcc_kernel_distributor_t {

    public:
	dcc_kernel_distributor_t() {}

	dcc_kernel_distributor_t(kernel_info_t * _kernel_grid, /*function_info * _kernel_entry,*/
		unsigned int _thread_count,
		unsigned int _optimal_block_size,
		unsigned int _optimal_kernel_size,
		void * _parameter_buffer,
		int _agg_group_id,
		dim3 _ctaid):
	    valid(false),
	    candidate(false),
	    launched(false),
	    kernel_grid(_kernel_grid),
	    thread_count(_thread_count),
	    optimal_block_size(_optimal_block_size),
	    optimal_kernel_size(_optimal_kernel_size),
	    parameter_buffer(_parameter_buffer),
	    merge_count(1),
	    stream(NULL),
            agg_group_id(_agg_group_id),
            ctaid(_ctaid),
   	    expected_launch_time(0),
   	    offset_base(0xFFFFFFFF){}

	bool valid, candidate, launched;
	kernel_info_t *kernel_grid;
	unsigned int thread_count;
	unsigned int optimal_block_size;
	unsigned int optimal_kernel_size;
	void *parameter_buffer;
	unsigned int merge_count;
	CUstream_st * stream; 
	int agg_group_id;
	dim3 ctaid;
	unsigned long long expected_launch_time;
	unsigned int offset_base;
	int kernel_queue_entry_id;
	unsigned int onchip_metadata;
};
extern bool g_dyn_child_thread_consolidation;	
extern unsigned g_dcc_timeout_threshold;
extern unsigned g_dyn_child_thread_consolidation_version;
extern bool g_restrict_parent_block_count;
extern bool g_simultaneous_multikernel_within_SM;

//Po-Han: hand-coded application ids for DCC
typedef enum _application_id {
    BFS,
    MST,
    JOIN,
    SSSP,
    COLOR,
    MIS,
    PAGERANK,
    KMEANS,
    SP,
    BC,
    SPMV,
    BL
} application_id;
//extern application_id g_app_name;
//extern std::list<dcc_kernel_distributor_t *> g_cuda_dcc_kernel_distributor;
bool compare_dcc_kd_entry(const dcc_kernel_distributor_t &a, const dcc_kernel_distributor_t &b);

void gpgpusim_cuda_getParameterBufferV2(const ptx_instruction * pI, ptx_thread_info * thread, const function_info * target_func);
void gpgpusim_cuda_launchDeviceV2(const ptx_instruction * pI, ptx_thread_info * thread, const function_info * target_func);
void gpgpusim_cuda_streamCreateWithFlags(const ptx_instruction * pI, ptx_thread_info * thread, const function_info * target_func);
void launch_all_device_kernels();
void launch_one_device_kernel(bool no_more_kernel, kernel_info_t *fin_parent, ptx_thread_info *sync_parent_thread);
void generate_one_consolidated_kernel(kernel_info_t *fin_parent, ptx_thread_info *sync_parent_thread);
void try_launch_child_kernel();
kernel_info_t * find_launched_grid(function_info * kernel_entry, kernel_info_t *parent_kernel, unsigned parent_block_idx);
//kernel_info_t * find_launched_grid(function_info * kernel_entry);

void gpgpusim_cuda_deviceSynchronize(const ptx_instruction * pI, ptx_thread_info * thread, const function_info * target_func);
bool merge_two_kernel_distributor_entry(dcc_kernel_distributor_t *kd_entry_1, dcc_kernel_distributor_t *kd_entry_2, bool ForceMerge, int target_size, bool &remaining);
bool is_target_parent_kernel(kernel_info_t *kernel);
