// Copyright (c) 2009-2011, Tor M. Aamodt, Inderpreet Singh,
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice, this
// list of conditions and the following disclaimer in the documentation and/or
// other materials provided with the distribution.
// Neither the name of The University of British Columbia nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef ABSTRACT_HARDWARE_MODEL_INCLUDED
#define ABSTRACT_HARDWARE_MODEL_INCLUDED

// Forward declarations
class gpgpu_sim;
class kernel_info_t;

//Set a hard limit of 32 CTAs per shader [cuda only has 8]
#define MAX_CTA_PER_SHADER 32
#define MAX_BARRIERS_PER_CTA 16

enum _memory_space_t {
	undefined_space=0,
	reg_space,
	local_space,
	shared_space,
	param_space_unclassified,
	param_space_kernel,  /* global to all threads in a kernel : read-only */
	param_space_local,   /* local to a thread : read-writable */
	const_space,
	tex_space,
	surf_space,
	global_space,
	generic_space,
	instruction_space,
	child_param_space	/* Po-Han: special space for child-kernel parameters */
};


enum FuncCache
{
	FuncCachePreferNone = 0,
	FuncCachePreferShared = 1,
	FuncCachePreferL1 = 2
};

#ifdef __cplusplus

#include <string.h>
#include <stdio.h>

typedef unsigned long long new_addr_type;
typedef unsigned address_type;
typedef unsigned addr_t;

// the following are operations the timing model can see 

enum uarch_op_t {
	NO_OP=-1,
	ALU_OP=1,
	SFU_OP,
	ALU_SFU_OP,
	LOAD_OP,
	STORE_OP,
	BRANCH_OP,
	BARRIER_OP,
	MEMORY_BARRIER_OP,
	CALL_OPS,
	RET_OPS
};
typedef enum uarch_op_t op_type;


enum uarch_bar_t {
	NOT_BAR=-1,
	SYNC=1,
	ARRIVE,
	RED
};
typedef enum uarch_bar_t barrier_type;

enum uarch_red_t {
	NOT_RED=-1,
	POPC_RED=1,
	AND_RED,
	OR_RED
};
typedef enum uarch_red_t reduction_type;


enum uarch_operand_type_t {
	UN_OP=-1,
	INT_OP,
	FP_OP
};
typedef enum uarch_operand_type_t types_of_operands;

enum special_operations_t {
	OTHER_OP,
	INT__OP,
	INT_MUL24_OP,
	INT_MUL32_OP,
	INT_MUL_OP,
	INT_DIV_OP,
	FP_MUL_OP,
	FP_DIV_OP,
	FP__OP,
	FP_SQRT_OP,
	FP_LG_OP,
	FP_SIN_OP,
	FP_EXP_OP
};
typedef enum special_operations_t special_ops; // Required to identify for the power model
enum operation_pipeline_t {
	UNKOWN_OP,
	SP__OP,
	SFU__OP,
	MEM__OP
};
typedef enum operation_pipeline_t operation_pipeline;
enum mem_operation_t {
	NOT_TEX,
	TEX
};
typedef enum mem_operation_t mem_operation;

enum _memory_op_t {
	no_memory_op = 0,
	memory_load,
	memory_store
};

#include <bitset>
#include <list>
#include <vector>
#include <assert.h>
#include <stdlib.h>
#include <map>
#include <deque>
#include <algorithm>
#include <set>

// dekline
typedef std::bitset<64> warp_set_t;
typedef std::bitset<2048/*1536*/> thread2block;

#if !defined(__VECTOR_TYPES_H__)
struct dim3 {
	unsigned int x, y, z;
};
#endif
bool increment_x_then_y_then_z( dim3 &i, const dim3 &bound);

//Jin: child kernel information for CDP
#include "stream_manager.h"
class stream_manager;
struct CUstream_st;
extern stream_manager * g_stream_manager;
//Jin: aggregated block group
class agg_block_group_t;
class ptx_thread_info;

// dekline
// Recording per block state for the kernel
struct block_info {
	// check if the block is issued
	unsigned issued;

	// to see if this block finishes
	unsigned done;

	// if this block is switched
	unsigned switched;

	// if this block had been preempted
	unsigned preempted;

        // if this block can be issued again (parent-child dependency)
	unsigned reissue;

	// time stamp for switching
	unsigned long long time_stamp_switching;

	// time stamp for switching issue
	unsigned long long time_stamp_switching_issue;

	// thread to cta mapping
	thread2block thread;

	// global id of this block
	dim3 block_id;

	unsigned cluster_id, shader_id, hw_cta_id;
	bool devsynced;
};

//Andrew
class shd_warp_t;
class simt_stack;
class thread_ctx_t;
class ptx_thread_info;
class ptx_cta_info;
class memory_space;

class switched_out_cta{
	public: 
		switched_out_cta()
		{
			m_warp_active.reset();
			m_warp_at_barrier.reset();
		}
		unsigned id;
		unsigned cta_size;
		unsigned num_warps;
		shd_warp_t* m_warp; //static
		simt_stack** m_simt_stack;
		thread_ctx_t* m_threadState; //static
		ptx_thread_info** m_thread;
		ptx_cta_info* m_ptx_cta_info;
		memory_space* m_shared_memory;
		memory_space** m_local_memory;
		std::set<unsigned> *m_regs; //static
		std::set<unsigned> *m_longopregs; //static
		warp_set_t m_bar_id_to_warps[16]; //static
		warp_set_t m_cta_to_warps; //static
		warp_set_t m_warp_active; //static
		warp_set_t m_warp_at_barrier; //static
};

class kernel_info_t {
	public:
		//   kernel_info_t()
		//   {
		//      m_valid=false;
		//      m_kernel_entry=NULL;
		//      m_uid=0;
		//      m_num_cores_running=0;
		//      m_param_mem=NULL;
		//   }
		kernel_info_t( dim3 gridDim, dim3 blockDim, class function_info *entry );
		~kernel_info_t();

		void inc_running() { m_num_cores_running++; }
		void dec_running()
		{
			assert( m_num_cores_running > 0 );
			m_num_cores_running--; 
		}
		bool running() const { return m_num_cores_running>0; }
		bool done() const 
		{
			return no_more_ctas_to_run() && !running() && (preempted_list.size() == 0);
		}
		class function_info *entry() { return m_kernel_entry; }
		const class function_info *entry() const { return m_kernel_entry; }

		size_t num_blocks() const
		{
			size_t native_num_blocks = m_grid_dim.x * m_grid_dim.y * m_grid_dim.z;

			return native_num_blocks + m_total_num_agg_blocks;
		}

		size_t threads_per_cta() const
		{
			return m_block_dim.x * m_block_dim.y * m_block_dim.z;
		} 

		dim3 get_grid_dim(int agg_group_id) const { 
			if(agg_group_id == -1) //native kernel
				return m_grid_dim; 
			else
				return get_agg_dim(agg_group_id);
		}
		dim3 get_cta_dim() const { return m_block_dim; }
		//Po-Han: DCC implementation
		void set_grid_dim(dim3 gridDim) { m_grid_dim = gridDim; }

		void increment_cta_id();
#if 0
		{ 
		    dim3 next_grid_dim = get_grid_dim(m_next_agg_group_id);
		    if(increment_x_then_y_then_z(m_next_cta, next_grid_dim)) { //overbound
			m_next_agg_group_id++;
			m_next_cta.x = 0;
			m_next_cta.y = 0;
			m_next_cta.z = 0;

			extern bool g_dcc_kernel_param_onchip;
			if( g_dcc_kernel_param_onchip && is_child ){
			    extern signed kernel_param_usage;
			    extern signed long long param_buffer_usage;
			    extern unsigned g_max_param_buffer_size;
			    extern unsigned g_param_buffer_thres_low;
			    extern bool param_buffer_full; 
			    param_buffer_usage -= kernel_param_usage;
			    if(param_buffer_usage < 0) param_buffer_usage = 0;
			    fprintf(stdout, "KPM: Clear an entry, param_buffer usage %lld", param_buffer_usage);
			    if(param_buffer_usage * 100 < g_max_param_buffer_size * g_param_buffer_thres_low){
				param_buffer_full = false;
				fprintf(stdout, ", <%u\%, turn-off full bit", g_param_buffer_thres_low);
			    }
			    fprintf(stdout, "\n");
			}
			extern bool g_estimate_offchip_metadata_load_latency;
			if( g_estimate_offchip_metadata_load_latency ){
			    if( m_next_agg_group_id < m_total_agg_group_id ){ //move to the next agg group
				if( m_agg_block_groups.find(m_next_agg_group_id)->second->get_kernel_queue_entry_id() == -1 ){ //next agg group info is stored in global memory
				    //				extern bool extra_metadata_load_latency;
				    extra_metadata_load_latency = true;
				}
			    }
			}
		    }

		    m_next_tid.x=0;
		    m_next_tid.y=0;
		    m_next_tid.z=0;
		}
#endif
		dim3 get_next_cta_id() const { return m_next_cta; }
		bool no_more_ctas_to_run() const 
		{
			return m_next_agg_group_id >= m_total_agg_group_id;
			//return (m_next_cta.x >= m_grid_dim.x || m_next_cta.y >= m_grid_dim.y || m_next_cta.z >= m_grid_dim.z );

		}

		void increment_thread_id() { increment_x_then_y_then_z(m_next_tid,m_block_dim); }
		dim3 get_next_thread_id_3d() const  { return m_next_tid; }
		unsigned get_next_thread_id() const 
		{ 
			return m_next_tid.x + m_block_dim.x*m_next_tid.y + m_block_dim.x*m_block_dim.y*m_next_tid.z;
		}
		unsigned get_next_block_id() const
	        {
	           return m_next_cta.x + m_grid_dim.x*m_next_cta.y + m_grid_dim.x*m_grid_dim.y*m_next_cta.z;
                }
		bool more_threads_in_cta() const 
		{
			return m_next_tid.z < m_block_dim.z && m_next_tid.y < m_block_dim.y && m_next_tid.x < m_block_dim.x;
		}
		bool last_block() const
		{
		    return (m_next_cta.x +1 == m_grid_dim.x) && (m_next_cta.y +1 == m_grid_dim.y) && (m_next_cta.z +1 == m_grid_dim.z);
		}
		unsigned get_uid() const { return m_uid; }
		std::string name() const;

		std::list<class ptx_thread_info *> &active_threads() { return m_active_threads; }
		class memory_space *get_param_memory(int agg_group_id) { 

			if(agg_group_id == -1) //native
				return m_param_mem; 
			else {
				return get_agg_param_mem(agg_group_id);
			}
		}
		addr_t get_param_memory_base(int agg_group_id) {
			if(agg_group_id == -1)
				return m_param_mem_base;
			else {
				return get_agg_param_mem_base(agg_group_id);
			}
		}

		unsigned get_child_count() const { return m_child_kernels.size(); }
		void set_param_mem(class memory_space *p_mem) 
		{ 
		   m_param_mem = p_mem; 
		}

		//bddream - DKPL
		void set_param_mem_base(addr_t p_mem_base){
			m_param_mem_base = p_mem_base;
		}

		//Andrew
		std::map<unsigned, switched_out_cta>& getSwitchedCTA(){return m_switched_out_cta;};
		void insertSwitchedCTA(unsigned cta_id, switched_out_cta& cta){m_switched_out_cta[cta_id] = cta;};

	private:
		kernel_info_t( const kernel_info_t & ); // disable copy constructor
		void operator=( const kernel_info_t & ); // disable copy operator

		class function_info *m_kernel_entry;

		unsigned m_uid;
		static unsigned m_next_uid;

		dim3 m_grid_dim;
		dim3 m_block_dim;
		dim3 m_next_cta;
		dim3 m_next_tid;

		unsigned m_num_cores_running;

		std::list<class ptx_thread_info *> m_active_threads;
		class memory_space *m_param_mem;
		//bddream - DKPL
		//simulate device-launched kernel parameter read latency
		addr_t m_param_mem_base;

		//Andrew
		std::map<unsigned, switched_out_cta> m_switched_out_cta; //use global block ID to index switched out blocks

	public:
		//Jin: parent and child kernel management for CDP
		void set_parent(kernel_info_t * parent, int parent_agg_group_id, dim3 parent_ctaid, dim3 parent_tid, unsigned parent_block_idx, unsigned parent_thread_idx);
		void add_parent(kernel_info_t * parent, ptx_thread_info * thread);
		void set_child(kernel_info_t * child);
		void remove_child(kernel_info_t * child);
		bool is_finished();
		bool children_all_finished();
		void notify_parent_finished();
		CUstream_st * create_stream_cta(int agg_group_id, dim3 ctaid);
		CUstream_st * get_default_stream_cta(int agg_group_id, dim3 ctaid);
		bool cta_has_stream(int agg_group_id, dim3 ctaid, CUstream_st* stream);
		void delete_stream_cta(int agg_group_id, dim3 ctaid, CUstream_st* stream);
		void print_parent_info();
		void destroy_cta_streams();
		kernel_info_t * get_parent() { return m_parent_kernel; }
		unsigned get_parent_block_idx() { return m_parent_block_idx; }
		std::list<ptx_thread_info *> m_parent_threads; //Po-Han: parent threads
		//Po-Han DCC
		std::map<unsigned int, class memory_space *> m_param_mem_map;
		std::map<unsigned int, addr_t> m_param_mem_base_map;
		std::map<unsigned int, int> m_kernel_queue_entry_map;
		bool is_child;
		signed int param_entry_cnt;
		unsigned **per_SM_block_cnt;
		bool extra_metadata_load_latency; 
		unsigned long long next_dispatchable_cycle;
		unsigned int metadata_count;
		unsigned int unissued_agg_groups;

		// dekline
		struct block_info *block_state;
//		bool switched_done() const;
//		unsigned switched_done;
		unsigned find_block_idx( dim3 block_id );
		void reset_block_state(); //Po-Han DCC
		bool parent_child_dependency;
		std::list<unsigned int> preswitch_list;
		std::list<unsigned int> switching_list;
		std::list<unsigned int> preempted_list;
		int get_kernel_queue_entry(int agg_group_id);

		bool no_more_block_to_run()
		{
			return ( m_next_cta.x >= m_grid_dim.x || m_next_cta.y >= m_grid_dim.y || m_next_cta.z >= m_grid_dim.z );
		}

		void increment_block_id() 
		{ 
			increment_x_then_y_then_z(m_next_cta,m_grid_dim); 
		}

	private:
		kernel_info_t * m_parent_kernel;
		int m_parent_agg_group_id; // -1 means launched by native parent ctas
		dim3 m_parent_ctaid;
		unsigned m_parent_block_idx;
		dim3 m_parent_tid;
		unsigned m_parent_thread_idx;
		std::list<kernel_info_t *> m_child_kernels; //child kernel launched
		//   std::list<ptx_thread_info *> m_parent_threads; //Po-Han: parent threads
		//streams created in each CTA, indexed by (int agg_group_id, dim3 cta_id)
		typedef std::pair<int, dim3> agg_block_id_t;
		struct agg_block_id_comp {
			bool operator() (const agg_block_id_t & a, const agg_block_id_t & b) const
			{   
				/*if(a.first < b.first)
					return true;
				else if(a.second.z < b.second.z)
					return true;
				else if(a.second.y < b.second.y)
					return true;
				else if (a.second.x < b.second.x)
					return true;
				else
					return false;*/
				if(a.first < b.first){
					return true;
				}else if(a.first == b.first){
					if(a.second.z < b.second.z){
						return true;
					}else if(a.second.z == b.second.z){
						if(a.second.y < b.second.y){
							return true;
						}else if(a.second.y == b.second.y){
							if (a.second.x < b.second.x){
								return true;
							}else{
								return false;
							}
						}else{
							return false;
						}
					}else{
						return false;
					}
				}else{
					return false;
				}
			}
		};

		std::map< agg_block_id_t, std::list<CUstream_st *>, agg_block_id_comp> m_cta_streams; 
		//Jin: aggregated cta management
	public:
		void add_agg_block_group(agg_block_group_t * agg_block_group);
		void destroy_agg_block_groups();
		dim3 get_agg_dim(int agg_group_id) const;
		class memory_space * get_agg_param_mem(int agg_group_id);
		addr_t get_agg_param_mem_base(int agg_group_id);
		int get_next_agg_group_id() { return m_next_agg_group_id; }
	private:
		int m_next_agg_group_id;
		int m_total_agg_group_id;
		std::map<int, agg_block_group_t *> m_agg_block_groups; //aggregated block groups
		size_t m_total_num_agg_blocks; //total number of aggregated blocks
		//Jin: kernel timing
	public:
		unsigned long long launch_cycle;
		unsigned long long start_cycle;
		unsigned long long end_cycle;
		unsigned m_launch_latency;
};

struct core_config {
	core_config() 
	{ 
		m_valid = false; 
		num_shmem_bank=16; 
		shmem_limited_broadcast = false; 
		gpgpu_shmem_sizeDefault=(unsigned)-1;
		gpgpu_shmem_sizePrefL1=(unsigned)-1;
		gpgpu_shmem_sizePrefShared=(unsigned)-1;
	}
	virtual void init() = 0;

	bool m_valid;
	unsigned warp_size;

	// off-chip memory request architecture parameters
	int gpgpu_coalesce_arch;

	// shared memory bank conflict checking parameters
	bool shmem_limited_broadcast;
	static const address_type WORD_SIZE=4;
	unsigned num_shmem_bank;
	unsigned shmem_bank_func(address_type addr) const
	{
		return ((addr/WORD_SIZE) % num_shmem_bank);
	}
	unsigned mem_warp_parts;  
	mutable unsigned gpgpu_shmem_size;
	unsigned gpgpu_shmem_sizeDefault;
	unsigned gpgpu_shmem_sizePrefL1;
	unsigned gpgpu_shmem_sizePrefShared;

	// texture and constant cache line sizes (used to determine number of memory accesses)
	unsigned gpgpu_cache_texl1_linesize;
	unsigned gpgpu_cache_constl1_linesize;

	unsigned gpgpu_max_insn_issue_per_warp;
};

// bounded stack that implements simt reconvergence using pdom mechanism from MICRO'07 paper
const unsigned MAX_WARP_SIZE = 32;
typedef std::bitset<MAX_WARP_SIZE> active_mask_t;
#define MAX_WARP_SIZE_SIMT_STACK  MAX_WARP_SIZE
typedef std::bitset<MAX_WARP_SIZE_SIMT_STACK> simt_mask_t;
typedef std::vector<address_type> addr_vector_t;

class simt_stack {
	public:
		simt_stack( unsigned wid,  unsigned warpSize);

		void reset();
		void launch( address_type start_pc, const simt_mask_t &active_mask );
		void update( simt_mask_t &thread_done, addr_vector_t &next_pc, address_type recvg_pc, op_type next_inst_op,unsigned next_inst_size, address_type next_inst_pc );

		const simt_mask_t &get_active_mask() const;
		void     get_pdom_stack_top_info( unsigned *pc, unsigned *rpc ) const;
		unsigned get_rp() const;
		void     print(FILE*fp) const;

		//Andrew
		unsigned m_kernel_id;
		unsigned m_warp_id;
		unsigned m_cta_id;
		unsigned m_global_cta_id;
		enum stack_entry_type {
			STACK_ENTRY_TYPE_NORMAL = 0,
			STACK_ENTRY_TYPE_CALL
		};
		struct simt_stack_entry {
			address_type m_pc;
			unsigned int m_calldepth;
			simt_mask_t m_active_mask;
			address_type m_recvg_pc;
			unsigned long long m_branch_div_cycle;
			stack_entry_type m_type;
			simt_stack_entry() :
				m_pc(-1), m_calldepth(0), m_active_mask(), m_recvg_pc(-1), m_branch_div_cycle(0), m_type(STACK_ENTRY_TYPE_NORMAL) { };
		};

		std::deque<simt_stack_entry> m_stack;

	protected:
		unsigned m_warp_size;
};

//Po-Han: dynamic child thread consolidation, reserve 4G space for global heap
#define CHILD_PARAM_START   0xE0000000
#define CHILD_PARAM_END	0xF0000000
//#define DCC_PARAM_START   0x180000000

#define GLOBAL_HEAP_START 0x80000000
// start allocating from this address (lower values used for allocating globals in .ptx file)
#define SHARED_MEM_SIZE_MAX (64*1024)
#define LOCAL_MEM_SIZE_MAX (8*1024)
#define MAX_STREAMING_MULTIPROCESSORS 64
#define MAX_THREAD_PER_SM 2048
#define TOTAL_LOCAL_MEM_PER_SM (MAX_THREAD_PER_SM*LOCAL_MEM_SIZE_MAX)
#define TOTAL_SHARED_MEM (MAX_STREAMING_MULTIPROCESSORS*SHARED_MEM_SIZE_MAX)
#define TOTAL_LOCAL_MEM (MAX_STREAMING_MULTIPROCESSORS*MAX_THREAD_PER_SM*LOCAL_MEM_SIZE_MAX)
#define SHARED_GENERIC_START (GLOBAL_HEAP_START-TOTAL_SHARED_MEM)
#define LOCAL_GENERIC_START (SHARED_GENERIC_START-TOTAL_LOCAL_MEM)
#define STATIC_ALLOC_LIMIT (GLOBAL_HEAP_START - (TOTAL_LOCAL_MEM+TOTAL_SHARED_MEM))

#if !defined(__CUDA_RUNTIME_API_H__)

enum cudaChannelFormatKind {
	cudaChannelFormatKindSigned,
	cudaChannelFormatKindUnsigned,
	cudaChannelFormatKindFloat
};

struct cudaChannelFormatDesc {
	int                        x;
	int                        y;
	int                        z;
	int                        w;
	enum cudaChannelFormatKind f;
};

struct cudaArray {
	void *devPtr;
	int devPtr32;
	struct cudaChannelFormatDesc desc;
	int width;
	int height;
	int size; //in bytes
	unsigned dimensions;
};

enum cudaTextureAddressMode {
	cudaAddressModeWrap,
	cudaAddressModeClamp
};

enum cudaTextureFilterMode {
	cudaFilterModePoint,
	cudaFilterModeLinear
};

enum cudaTextureReadMode {
	cudaReadModeElementType,
	cudaReadModeNormalizedFloat
};

struct textureReference {
	int                           normalized;
	enum cudaTextureFilterMode    filterMode;
	enum cudaTextureAddressMode   addressMode[3];
	struct cudaChannelFormatDesc  channelDesc;
};

#endif

// Struct that record other attributes in the textureReference declaration 
// - These attributes are passed thru __cudaRegisterTexture()
struct textureReferenceAttr {
	const struct textureReference *m_texref; 
	int m_dim; 
	enum cudaTextureReadMode m_readmode; 
	int m_ext; 
	textureReferenceAttr(const struct textureReference *texref, 
			int dim, 
			enum cudaTextureReadMode readmode, 
			int ext)
		: m_texref(texref), m_dim(dim), m_readmode(readmode), m_ext(ext) 
	{  }
};

class gpgpu_functional_sim_config 
{
	public:
		void reg_options(class OptionParser * opp);

		void ptx_set_tex_cache_linesize(unsigned linesize);

		unsigned get_forced_max_capability() const { return m_ptx_force_max_capability; }
		bool convert_to_ptxplus() const { return m_ptx_convert_to_ptxplus; }
		bool use_cuobjdump() const { return m_ptx_use_cuobjdump; }
		bool experimental_lib_support() const { return m_experimental_lib_support; }

		int         get_ptx_inst_debug_to_file() const { return g_ptx_inst_debug_to_file; }
		const char* get_ptx_inst_debug_file() const  { return g_ptx_inst_debug_file; }
		int         get_ptx_inst_debug_thread_uid() const { return g_ptx_inst_debug_thread_uid; }
		unsigned    get_texcache_linesize() const { return m_texcache_linesize; }

	private:
		// PTX options
		int m_ptx_convert_to_ptxplus;
		int m_ptx_use_cuobjdump;
		int m_experimental_lib_support;
		unsigned m_ptx_force_max_capability;

		int   g_ptx_inst_debug_to_file;
		char* g_ptx_inst_debug_file;
		int   g_ptx_inst_debug_thread_uid;

		unsigned m_texcache_linesize;
};

class gpgpu_t {
	public:
		gpgpu_t( const gpgpu_functional_sim_config &config );
		void* child_param_malloc( size_t size ); //Po-Han: dynamic child-thread consolidation
		void* gpu_malloc( size_t size );
		void* gpu_mallocarray( size_t count );
		void  gpu_memset( size_t dst_start_addr, int c, size_t count );
		void  memcpy_to_gpu( size_t dst_start_addr, const void *src, size_t count );
		void  memcpy_from_gpu( void *dst, size_t src_start_addr, size_t count );
		void  memcpy_gpu_to_gpu( size_t dst, size_t src, size_t count );

		class memory_space *get_global_memory() { return m_global_mem; }
		class memory_space *get_tex_memory() { return m_tex_mem; }
		class memory_space *get_surf_memory() { return m_surf_mem; }

		void gpgpu_ptx_sim_bindTextureToArray(const struct textureReference* texref, const struct cudaArray* array);
		void gpgpu_ptx_sim_bindNameToTexture(const char* name, const struct textureReference* texref, int dim, int readmode, int ext);
		const char* gpgpu_ptx_sim_findNamefromTexture(const struct textureReference* texref);

		const struct textureReference* get_texref(const std::string &texname) const
		{
			std::map<std::string, const struct textureReference*>::const_iterator t=m_NameToTextureRef.find(texname);
			if(t == m_NameToTextureRef.end())
				printf("%s\n", texname.c_str());
			assert( t != m_NameToTextureRef.end() );
			return t->second;
		}
		const struct cudaArray* get_texarray( const struct textureReference *texref ) const
		{
			std::map<const struct textureReference*,const struct cudaArray*>::const_iterator t=m_TextureRefToCudaArray.find(texref);
			assert(t != m_TextureRefToCudaArray.end());
			return t->second;
		}
		const struct textureInfo* get_texinfo( const struct textureReference *texref ) const
		{
			std::map<const struct textureReference*, const struct textureInfo*>::const_iterator t=m_TextureRefToTexureInfo.find(texref);
			assert(t != m_TextureRefToTexureInfo.end());
			return t->second;
		}

		const struct textureReferenceAttr* get_texattr( const struct textureReference *texref ) const
		{
			std::map<const struct textureReference*, const struct textureReferenceAttr*>::const_iterator t=m_TextureRefToAttribute.find(texref);
			assert(t != m_TextureRefToAttribute.end());
			return t->second;
		}

		const gpgpu_functional_sim_config &get_config() const { return m_function_model_config; }
		FILE* get_ptx_inst_debug_file() { return ptx_inst_debug_file; }

	protected:
		const gpgpu_functional_sim_config &m_function_model_config;
		FILE* ptx_inst_debug_file;

		class memory_space *m_global_mem;
		class memory_space *m_tex_mem;
		class memory_space *m_surf_mem;

		unsigned long long m_dev_malloc;

		//Po-Han: dynamic child kernel consolidation
		unsigned long long m_child_param_malloc;

		std::map<std::string, const struct textureReference*> m_NameToTextureRef;
		std::map<const struct textureReference*,const struct cudaArray*> m_TextureRefToCudaArray;
		std::map<const struct textureReference*, const struct textureInfo*> m_TextureRefToTexureInfo;
		std::map<const struct textureReference*, const struct textureReferenceAttr*> m_TextureRefToAttribute;
};

struct gpgpu_ptx_sim_kernel_info 
{
	// Holds properties of the kernel (Kernel's resource use). 
	// These will be set to zero if a ptxinfo file is not present.
	int lmem;
	int smem;
	int cmem;
	int regs;
	unsigned ptx_version;
	unsigned sm_target;
};

struct gpgpu_ptx_sim_arg {
	gpgpu_ptx_sim_arg() { m_start=NULL; }
	gpgpu_ptx_sim_arg(const void *arg, size_t size, size_t offset)
	{
		m_start=arg;
		m_nbytes=size;
		m_offset=offset;
	}
	const void *m_start;
	size_t m_nbytes;
	size_t m_offset;
};

typedef std::list<gpgpu_ptx_sim_arg> gpgpu_ptx_sim_arg_list_t;

class memory_space_t {
	public:
		memory_space_t() { m_type = undefined_space; m_bank=0; }
		memory_space_t( const enum _memory_space_t &from ) { m_type = from; m_bank = 0; }
		bool operator==( const memory_space_t &x ) const { return (m_bank == x.m_bank) && (m_type == x.m_type); }
		bool operator!=( const memory_space_t &x ) const { return !(*this == x); }
		bool operator<( const memory_space_t &x ) const 
		{ 
			if(m_type < x.m_type)
				return true;
			else if(m_type > x.m_type)
				return false;
			else if( m_bank < x.m_bank )
				return true; 
			return false;
		}
		enum _memory_space_t get_type() const { return m_type; }
		void set_type(const enum _memory_space_t &type) { m_type = type; }
		unsigned get_bank() const { return m_bank; }
		void set_bank( unsigned b ) { m_bank = b; }
		bool is_const() const { return (m_type == const_space) || (m_type == param_space_kernel); }
		bool is_local() const { return (m_type == local_space) || (m_type == param_space_local); }
		bool is_global() const { return (m_type == global_space); }

	private:
		enum _memory_space_t m_type;
		unsigned m_bank; // n in ".const[n]"; note .const == .const[0] (see PTX 2.1 manual, sec. 5.1.3)
};

const unsigned MAX_MEMORY_ACCESS_SIZE = 128;
typedef std::bitset<MAX_MEMORY_ACCESS_SIZE> mem_access_byte_mask_t;
#define NO_PARTIAL_WRITE (mem_access_byte_mask_t())

#define MEM_ACCESS_TYPE_TUP_DEF \
	MA_TUP_BEGIN( mem_access_type ) \
MA_TUP( GLOBAL_ACC_R ), \
MA_TUP( LOCAL_ACC_R ), \
MA_TUP( CONST_ACC_R ), \
MA_TUP( TEXTURE_ACC_R ), \
MA_TUP( GLOBAL_ACC_W ), \
MA_TUP( LOCAL_ACC_W ), \
MA_TUP( L1_WRBK_ACC ), \
MA_TUP( L2_WRBK_ACC ), \
MA_TUP( INST_ACC_R ), \
MA_TUP( L1_WR_ALLOC_R ), \
MA_TUP( L2_WR_ALLOC_R ), \
MA_TUP( NUM_MEM_ACCESS_TYPE ) \
MA_TUP_END( mem_access_type ) 

#define MA_TUP_BEGIN(X) enum X {
#define MA_TUP(X) X
#define MA_TUP_END(X) };
MEM_ACCESS_TYPE_TUP_DEF
#undef MA_TUP_BEGIN
#undef MA_TUP
#undef MA_TUP_END

const char * mem_access_type_str(enum mem_access_type access_type); 

enum cache_operator_type {
	CACHE_UNDEFINED, 

	// loads
	CACHE_ALL,          // .ca
	CACHE_LAST_USE,     // .lu
	CACHE_VOLATILE,     // .cv

	// loads and stores 
	CACHE_STREAMING,    // .cs
	CACHE_GLOBAL,       // .cg

	// stores
	CACHE_WRITE_BACK,   // .wb
	CACHE_WRITE_THROUGH // .wt
};

class mem_access_t {
	public:
		mem_access_t() { init(); }
		mem_access_t( mem_access_type type, 
				new_addr_type address, 
				unsigned size,
				bool wr )
		{
			init();
			m_type = type;
			m_addr = address;
			m_req_size = size;
			m_write = wr;
		}
		mem_access_t( mem_access_type type, 
				new_addr_type address, 
				unsigned size, 
				bool wr, 
				const active_mask_t &active_mask,
				const mem_access_byte_mask_t &byte_mask )
			: m_warp_mask(active_mask), m_byte_mask(byte_mask)
		{
			init();
			m_type = type;
			m_addr = address;
			m_req_size = size;
			m_write = wr;
		}

		new_addr_type get_addr() const { return m_addr; }
		void set_addr(new_addr_type addr) {m_addr=addr;}
		unsigned get_size() const { return m_req_size; }
		const active_mask_t &get_warp_mask() const { return m_warp_mask; }
		bool is_write() const { return m_write; }
		enum mem_access_type get_type() const { return m_type; }
		mem_access_byte_mask_t get_byte_mask() const { return m_byte_mask; }

		void print(FILE *fp) const
		{
			fprintf(fp,"addr=0x%llx, %s, size=%u, ", m_addr, m_write?"store":"load ", m_req_size );
			switch(m_type) {
				case GLOBAL_ACC_R:  fprintf(fp,"GLOBAL_R"); break;
				case LOCAL_ACC_R:   fprintf(fp,"LOCAL_R "); break;
				case CONST_ACC_R:   fprintf(fp,"CONST   "); break;
				case TEXTURE_ACC_R: fprintf(fp,"TEXTURE "); break;
				case GLOBAL_ACC_W:  fprintf(fp,"GLOBAL_W"); break;
				case LOCAL_ACC_W:   fprintf(fp,"LOCAL_W "); break;
				case L2_WRBK_ACC:   fprintf(fp,"L2_WRBK "); break;
				case INST_ACC_R:    fprintf(fp,"INST    "); break;
				case L1_WRBK_ACC:   fprintf(fp,"L1_WRBK "); break;
				default:            fprintf(fp,"unknown "); break;
			}
		}

	private:
		void init() 
		{
			m_uid=++sm_next_access_uid;
			m_addr=0;
			m_req_size=0;
		}

		unsigned      m_uid;
		new_addr_type m_addr;     // request address
		bool          m_write;
		unsigned      m_req_size; // bytes
		mem_access_type m_type;
		active_mask_t m_warp_mask;
		mem_access_byte_mask_t m_byte_mask;

		static unsigned sm_next_access_uid;
};

class mem_fetch;

class mem_fetch_interface {
	public:
		virtual bool full( unsigned size, bool write ) const = 0;
		virtual void push( mem_fetch *mf ) = 0;
};

class mem_fetch_allocator {
	public:
		virtual mem_fetch *alloc( new_addr_type addr, mem_access_type type, unsigned size, bool wr ) const = 0;
		virtual mem_fetch *alloc( const class warp_inst_t &inst, const mem_access_t &access ) const = 0;
};

// the maximum number of destination, source, or address uarch operands in a instruction
#define MAX_REG_OPERANDS 8

struct dram_callback_t {
	dram_callback_t() { function=NULL; instruction=NULL; thread=NULL; }
	void (*function)(const class inst_t*, class ptx_thread_info*);
	const class inst_t* instruction;
	class ptx_thread_info *thread;
};

class inst_t {
	public:
		inst_t()
		{
			m_decoded=false;
			pc=(address_type)-1;
			reconvergence_pc=(address_type)-1;
			op=NO_OP;
			bar_type=NOT_BAR;
			red_type=NOT_RED;
			bar_id=(unsigned)-1;
			bar_count=(unsigned)-1;
			oprnd_type=UN_OP;
			sp_op=OTHER_OP;
			op_pipe=UNKOWN_OP;
			mem_op=NOT_TEX;
			num_operands=0;
			num_regs=0;
			memset(out, 0, sizeof(unsigned)); 
			memset(in, 0, sizeof(unsigned)); 
			is_vectorin=0; 
			is_vectorout=0;
			space = memory_space_t();
			cache_op = CACHE_UNDEFINED;
			latency = 1;
			initiation_interval = 1;
			for( unsigned i=0; i < MAX_REG_OPERANDS; i++ ) {
				arch_reg.src[i] = -1;
				arch_reg.dst[i] = -1;
			}
			isize=0;
		}
		bool valid() const { return m_decoded; }
		virtual void print_insn( FILE *fp ) const 
		{
			fprintf(fp," [inst @ pc=0x%04x] ", pc );
		}
		bool is_load() const { return (op == LOAD_OP || memory_op == memory_load); }
		bool is_store() const { return (op == STORE_OP || memory_op == memory_store); }
		unsigned get_num_operands() const {return num_operands;}
		unsigned get_num_regs() const {return num_regs;}
		void set_num_regs(unsigned num) {num_regs=num;}
		void set_num_operands(unsigned num) {num_operands=num;}
		void set_bar_id(unsigned id) {bar_id=id;}
		void set_bar_count(unsigned count) {bar_count=count;}

		address_type pc;        // program counter address of instruction
		unsigned isize;         // size of instruction in bytes 
		op_type op;             // opcode (uarch visible)

		barrier_type bar_type;
		reduction_type red_type;
		unsigned bar_id;
		unsigned bar_count;

		types_of_operands oprnd_type;     // code (uarch visible) identify if the operation is an interger or a floating point
		special_ops sp_op;           // code (uarch visible) identify if int_alu, fp_alu, int_mul ....
		operation_pipeline op_pipe;  // code (uarch visible) identify the pipeline of the operation (SP, SFU or MEM)
		mem_operation mem_op;        // code (uarch visible) identify memory type
		_memory_op_t memory_op; // memory_op used by ptxplus 
		unsigned num_operands;
		unsigned num_regs; // count vector operand as one register operand

		address_type reconvergence_pc; // -1 => not a branch, -2 => use function return address

		unsigned out[4];
		unsigned in[4];
		unsigned char is_vectorin;
		unsigned char is_vectorout;
		int pred; // predicate register number
		int ar1, ar2;
		// register number for bank conflict evaluation
		struct {
			int dst[MAX_REG_OPERANDS];
			int src[MAX_REG_OPERANDS];
		} arch_reg;
		//int arch_reg[MAX_REG_OPERANDS]; // register number for bank conflict evaluation
		unsigned latency; // operation latency 
		unsigned initiation_interval;

		unsigned data_size; // what is the size of the word being operated on?
		memory_space_t space;
		cache_operator_type cache_op;

	protected:
		bool m_decoded;
		virtual void pre_decode() {}
};

enum divergence_support_t {
	POST_DOMINATOR = 1,
	NUM_SIMD_MODEL
};

const unsigned MAX_ACCESSES_PER_INSN_PER_THREAD = 8;

class warp_inst_t: public inst_t {
	public:
		// constructors
		warp_inst_t() 
		{
			m_uid=0;
			m_empty=true; 
			m_config=NULL; 
			warp_kernel = NULL;
		}
		warp_inst_t( const core_config *config/*, kernel_info_t *kernel*/ ) 
		{ 
			m_uid=0;
			assert(config->warp_size<=MAX_WARP_SIZE); 
			m_config=config;
			m_empty=true; 
			m_isatomic=false;
			m_per_scalar_thread_valid=false;
			m_mem_accesses_created=false;
			m_cache_hit=false;
			m_is_printf=false;
			m_is_cdp = 0;

//			warp_kernel = kernel;
		}
		virtual ~warp_inst_t(){
		}

		// modifiers
		void broadcast_barrier_reduction( const active_mask_t& access_mask);
		void do_atomic(bool forceDo=false);
		void do_atomic( const active_mask_t& access_mask, bool forceDo=false );
		inline void clear() 
		{ 
			m_empty=true; 
		}
		void issue( const active_mask_t &mask, unsigned warp_id, unsigned long long cycle, int dynamic_warp_id, bool child ) 
		{
			m_warp_active_mask = mask;
			m_warp_issued_mask = mask; 
			m_uid = ++sm_next_uid;
			m_warp_id = warp_id;
			m_dynamic_warp_id = dynamic_warp_id;
			issue_cycle = cycle;
			cycles = initiation_interval;
			m_cache_hit=false;
			m_empty=false;
			is_child = child;
		}
		const active_mask_t & get_active_mask() const
		{
			return m_warp_active_mask;
		}
		void completed( unsigned long long cycle ) const;  // stat collection: called when the instruction is completed  
		void set_addr( unsigned n, new_addr_type addr ) 
		{
			if( !m_per_scalar_thread_valid ) {
				m_per_scalar_thread.resize(m_config->warp_size);
				m_per_scalar_thread_valid=true;
			}
			m_per_scalar_thread[n].memreqaddr[0] = addr;
		}
		void set_addr( unsigned n, new_addr_type* addr, unsigned num_addrs )
		{
			if( !m_per_scalar_thread_valid ) {
				m_per_scalar_thread.resize(m_config->warp_size);
				m_per_scalar_thread_valid=true;
			}
			assert(num_addrs <= MAX_ACCESSES_PER_INSN_PER_THREAD);
			for(unsigned i=0; i<num_addrs; i++)
				m_per_scalar_thread[n].memreqaddr[i] = addr[i];
		}

		struct transaction_info {
			std::bitset<4> chunks; // bitmask: 32-byte chunks accessed
			mem_access_byte_mask_t bytes;
			active_mask_t active; // threads in this transaction

			bool test_bytes(unsigned start_bit, unsigned end_bit) {
				for( unsigned i=start_bit; i<=end_bit; i++ )
					if(bytes.test(i))
						return true;
				return false;
			}
		};

		void generate_mem_accesses();
		void memory_coalescing_arch_13( bool is_write, mem_access_type access_type );
		void memory_coalescing_arch_13_atomic( bool is_write, mem_access_type access_type );
		void memory_coalescing_arch_13_reduce_and_send( bool is_write, mem_access_type access_type, const transaction_info &info, new_addr_type addr, unsigned segment_size );

		bool check_global_constant_sharing_bypass(new_addr_type addr, unsigned pc);
		void add_callback( unsigned lane_id, 
				void (*function)(const class inst_t*, class ptx_thread_info*),
				const inst_t *inst, 
				class ptx_thread_info *thread,
				bool atomic)
		{
			if( !m_per_scalar_thread_valid ) {
				m_per_scalar_thread.resize(m_config->warp_size);
				m_per_scalar_thread_valid=true;
				if(atomic) m_isatomic=true;
			}
			m_per_scalar_thread[lane_id].callback.function = function;
			m_per_scalar_thread[lane_id].callback.instruction = inst;
			m_per_scalar_thread[lane_id].callback.thread = thread;
		}
		void set_active( const active_mask_t &active );

		void clear_active( const active_mask_t &inactive );
		void set_not_active( unsigned lane_id );

		// accessors
		virtual void print_insn(FILE *fp) const 
		{
			fprintf(fp," [inst @ pc=0x%04x] ", pc );
			for (int i=(int)m_config->warp_size-1; i>=0; i--)
				fprintf(fp, "%c", ((m_warp_active_mask[i])?'1':'0') );
		}
		inline bool active( unsigned thread ) const { return m_warp_active_mask.test(thread); }
		unsigned active_count() const { return m_warp_active_mask.count(); }
		unsigned issued_count() const { assert(m_empty == false); return m_warp_issued_mask.count(); }  // for instruction counting 
		inline bool empty() const { return m_empty; }
		unsigned warp_id() const 
		{ 
			assert( !m_empty );
			return m_warp_id; 
		}
		unsigned dynamic_warp_id() const 
		{ 
			assert( !m_empty );
			return m_dynamic_warp_id; 
		}
		bool has_callback( unsigned n ) const
		{
			return m_warp_active_mask[n] && m_per_scalar_thread_valid && 
				(m_per_scalar_thread[n].callback.function!=NULL);
		}
		new_addr_type get_addr( unsigned n ) const
		{
			assert( m_per_scalar_thread_valid );
			return m_per_scalar_thread[n].memreqaddr[0];
		}

		bool isatomic() const { return m_isatomic; }

		unsigned warp_size() const { return m_config->warp_size; }

		bool accessq_empty() const { return m_accessq.empty(); }
		unsigned accessq_count() const { return m_accessq.size(); }
		const mem_access_t &accessq_back() { return m_accessq.back(); }
		void accessq_pop_back() { m_accessq.pop_back(); }

		bool dispatch_delay()
		{ 
			if( cycles > 0 ) 
				cycles--;
			return cycles > 0;
		}

		bool has_dispatch_delay(){
			return cycles > 0;
		}

		void print( FILE *fout ) const;
		unsigned get_uid() const { return m_uid; }

	protected:

		unsigned m_uid;
		bool m_empty;
		bool m_cache_hit;
		unsigned long long issue_cycle;
		unsigned cycles; // used for implementing initiation interval delay
		bool m_isatomic;
		bool m_is_printf;
		unsigned m_warp_id;
		unsigned m_dynamic_warp_id; 
		const core_config *m_config; 
		active_mask_t m_warp_active_mask; // dynamic active mask for timing model (after predication)
		active_mask_t m_warp_issued_mask; // active mask at issue (prior to predication test) -- for instruction counting

		struct per_thread_info {
			per_thread_info() {
				for(unsigned i=0; i<MAX_ACCESSES_PER_INSN_PER_THREAD; i++)
					memreqaddr[i] = 0;
			}
			dram_callback_t callback;
			new_addr_type memreqaddr[MAX_ACCESSES_PER_INSN_PER_THREAD]; // effective address, upto 8 different requests (to support 32B access in 8 chunks of 4B each)
		};
		bool m_per_scalar_thread_valid;
		std::vector<per_thread_info> m_per_scalar_thread;
		bool m_mem_accesses_created;
		std::list<mem_access_t> m_accessq;

		static unsigned sm_next_uid;

		//Jin: cdp support
	public:
		int m_is_cdp;

		kernel_info_t *warp_kernel;
		bool is_child;

};

//inline void move_warp( warp_inst_t *&dst, warp_inst_t *&src );
inline void move_warp( warp_inst_t *&dst, warp_inst_t *&src )
{
    assert( dst->empty() );
    warp_inst_t* temp = dst;
    dst = src;
    src = temp;
    src->clear();
}

size_t get_kernel_code_size( class function_info *entry );

/*
 * This abstract class used as a base for functional and performance and simulation, it has basic functional simulation
 * data structures and procedures. 
 */
class core_t {
	public:
		core_t( gpgpu_sim *gpu, 
				kernel_info_t *kernel,
				unsigned warp_size,
				unsigned threads_per_shader )
			: m_gpu( gpu ),
			m_kernel( kernel ),
			m_simt_stack( NULL ),
			m_thread( NULL ),
			m_warp_size( warp_size )
	{
		m_warp_count = threads_per_shader/m_warp_size;
		// Handle the case where the number of threads is not a
		// multiple of the warp size
		if ( threads_per_shader % m_warp_size != 0 ) {
			m_warp_count += 1;
		}
		assert( m_warp_count * m_warp_size > 0 );
		m_thread = ( ptx_thread_info** )
			calloc( m_warp_count * m_warp_size,
					sizeof( ptx_thread_info* ) );
		initilizeSIMTStack(m_warp_count,m_warp_size);

		for(unsigned i=0; i<MAX_CTA_PER_SHADER; i++){
			for(unsigned j=0; j<MAX_BARRIERS_PER_CTA; j++){
				reduction_storage[i][j]=0;
			}
		}

	}
		virtual ~core_t() { free(m_thread); }
		virtual void warp_exit( unsigned warp_id ) = 0;
		virtual bool warp_waiting_at_barrier( unsigned warp_id ) const = 0;
		virtual void checkExecutionStatusAndUpdate(warp_inst_t &inst, unsigned t, unsigned tid)=0;
		class gpgpu_sim * get_gpu() {return m_gpu;}
		void execute_warp_inst_t(warp_inst_t &inst, unsigned warpId =(unsigned)-1);
		bool  ptx_thread_done( unsigned hw_thread_id ) const ;
		void updateSIMTStack(unsigned warpId, warp_inst_t * inst);
		void initilizeSIMTStack(unsigned warp_count, unsigned warps_size);
		void deleteSIMTStack();
		warp_inst_t getExecuteWarp(unsigned warpId);
		void get_pdom_stack_top_info( unsigned warpId, unsigned *pc, unsigned *rpc ) const;
		kernel_info_t * get_kernel_info(){ return m_kernel;}
		unsigned get_warp_size() const { return m_warp_size; }
		void and_reduction(unsigned ctaid, unsigned barid, bool value) { reduction_storage[ctaid][barid] &= value; }
		void or_reduction(unsigned ctaid, unsigned barid, bool value) { reduction_storage[ctaid][barid] |= value; }
		void popc_reduction(unsigned ctaid, unsigned barid, bool value) { reduction_storage[ctaid][barid] += value;}
		unsigned get_reduction_value(unsigned ctaid, unsigned barid) {return reduction_storage[ctaid][barid];}
		// dekline
		class ptx_thread_info ** m_thread;
		simt_stack **getSimtStack(){return m_simt_stack;};

		unsigned long long next_dispatchable_cycle;
	protected:
		class gpgpu_sim *m_gpu;
		kernel_info_t *m_kernel;
		simt_stack  **m_simt_stack; // pdom based reconvergence context for each warp
		unsigned m_warp_size;
		unsigned m_warp_count;
		unsigned reduction_storage[MAX_CTA_PER_SHADER][MAX_BARRIERS_PER_CTA];
};


//register that can hold multiple instructions.
class register_set {
	public:
		register_set(unsigned num, const char* name){
			for( unsigned i = 0; i < num; i++ ) {
				regs.push_back(new warp_inst_t());
			}
			m_name = name;
		}
		bool has_free(){
			for( unsigned i = 0; i < regs.size(); i++ ) {
				if( regs[i]->empty() ) {
					return true;
				}
			}
			return false;
		}
		bool has_ready(){
			for( unsigned i = 0; i < regs.size(); i++ ) {
				if( not regs[i]->empty() ) {
					return true;
				}
			}
			return false;
		}

		void move_in( warp_inst_t *&src ){
			warp_inst_t** free = get_free();
			move_warp(*free, src);
		}
		//void copy_in( warp_inst_t* src ){
		//   src->copy_contents_to(*get_free());
		//}
		void move_out_to( warp_inst_t *&dest ){
			warp_inst_t **ready=get_ready();
			move_warp(dest, *ready);
		}

		warp_inst_t** get_ready(){
			warp_inst_t** ready;
			ready = NULL;
			for( unsigned i = 0; i < regs.size(); i++ ) {
				if( not regs[i]->empty() ) {
					if( ready and (*ready)->get_uid() < regs[i]->get_uid() ) {
						// ready is oldest
					} else {
						ready = &regs[i];
					}
				}
			}
			return ready;
		}

		void print(FILE* fp) const{
			fprintf(fp, "%s : @%p\n", m_name, this);
			for( unsigned i = 0; i < regs.size(); i++ ) {
				fprintf(fp, "     ");
				regs[i]->print(fp);
				fprintf(fp, "\n");
			}
		}

		warp_inst_t ** get_free(){
			for( unsigned i = 0; i < regs.size(); i++ ) {
				if( regs[i]->empty() ) {
					return &regs[i];
				}
			}
			assert(0 && "No free registers found");
			return NULL;
		}

	private:
		std::vector<warp_inst_t*> regs;
		const char* m_name;
};

#endif // #ifdef __cplusplus

#endif // #ifndef ABSTRACT_HARDWARE_MODEL_INCLUDED
