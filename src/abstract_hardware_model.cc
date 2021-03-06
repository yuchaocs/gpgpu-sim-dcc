// Copyright (c) 2009-2011, Tor M. Aamodt, Inderpreet Singh, Timothy Rogers,
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



#include "abstract_hardware_model.h"
#include "agg_block_group.h"
#include "cuda-sim/memory.h"
#include "cuda-sim/ptx_ir.h"
#include "cuda-sim/ptx-stats.h"
#include "cuda-sim/cuda-sim.h"
#include "gpgpu-sim/gpu-sim.h"
#include "option_parser.h"
#include <algorithm>
#include "cuda-sim/cuda_device_runtime.h"

unsigned mem_access_t::sm_next_access_uid = 0;   
unsigned warp_inst_t::sm_next_uid = 0;

/*inline void move_warp( warp_inst_t *&dst, warp_inst_t *&src )
{
    assert( dst->empty() );
    warp_inst_t* temp = dst;
    dst = src;
    src = temp;
    src->clear();
}*/


void gpgpu_functional_sim_config::reg_options(class OptionParser * opp)
{
    option_parser_register(opp, "-gpgpu_ptx_use_cuobjdump", OPT_BOOL,
	    &m_ptx_use_cuobjdump,
	    "Use cuobjdump to extract ptx and sass from binaries",
#if (CUDART_VERSION >= 4000)
	    "1"
#else
	    "0"
#endif
	    );
    option_parser_register(opp, "-gpgpu_experimental_lib_support", OPT_BOOL,
	    &m_experimental_lib_support,
	    "Try to extract code from cuda libraries [Broken because of unknown cudaGetExportTable]",
	    "0");
    option_parser_register(opp, "-gpgpu_ptx_convert_to_ptxplus", OPT_BOOL,
	    &m_ptx_convert_to_ptxplus,
	    "Convert SASS (native ISA) to ptxplus and run ptxplus",
	    "0");
    option_parser_register(opp, "-gpgpu_ptx_force_max_capability", OPT_UINT32,
	    &m_ptx_force_max_capability,
	    "Force maximum compute capability",
	    "0");
    option_parser_register(opp, "-gpgpu_ptx_inst_debug_to_file", OPT_BOOL, 
	    &g_ptx_inst_debug_to_file, 
	    "Dump executed instructions' debug information to file", 
	    "0");
    option_parser_register(opp, "-gpgpu_ptx_inst_debug_file", OPT_CSTR, &g_ptx_inst_debug_file, 
	    "Executed instructions' debug output file",
	    "inst_debug.txt");
    option_parser_register(opp, "-gpgpu_ptx_inst_debug_thread_uid", OPT_INT32, &g_ptx_inst_debug_thread_uid, 
	    "Thread UID for executed instructions' debug output", 
	    "1");
}

void gpgpu_functional_sim_config::ptx_set_tex_cache_linesize(unsigned linesize)
{
    m_texcache_linesize = linesize;
}

    gpgpu_t::gpgpu_t( const gpgpu_functional_sim_config &config )
: m_function_model_config(config)
{
//    m_global_mem = new memory_space_impl<8192>("global",1024*1024);
    m_global_mem = new memory_space_impl<8192>("global",64*1024);
    m_tex_mem = new memory_space_impl<8192>("tex",64*1024);
    m_surf_mem = new memory_space_impl<8192>("surf",64*1024);

    m_dev_malloc=GLOBAL_HEAP_START; 

    //Po-Han dynamic child-thread consolidation
    m_child_param_malloc=CHILD_PARAM_START;

    if(m_function_model_config.get_ptx_inst_debug_to_file() != 0) 
	ptx_inst_debug_file = fopen(m_function_model_config.get_ptx_inst_debug_file(), "w");
}

address_type line_size_based_tag_func(new_addr_type address, new_addr_type line_size)
{
    //gives the tag for an address based on a given line size
    return address & ~(line_size-1);
}

const char * mem_access_type_str(enum mem_access_type access_type)
{
#define MA_TUP_BEGIN(X) static const char* access_type_str[] = {
#define MA_TUP(X) #X
#define MA_TUP_END(X) };
    MEM_ACCESS_TYPE_TUP_DEF
#undef MA_TUP_BEGIN
#undef MA_TUP
#undef MA_TUP_END

	assert(access_type < NUM_MEM_ACCESS_TYPE); 

    return access_type_str[access_type]; 
}


void warp_inst_t::clear_active( const active_mask_t &inactive ) {
    active_mask_t test = m_warp_active_mask;
    test &= inactive;
    assert( test == inactive ); // verify threads being disabled were active
    m_warp_active_mask &= ~inactive;
}

void warp_inst_t::set_not_active( unsigned lane_id ) {
    m_warp_active_mask.reset(lane_id);
}

void warp_inst_t::set_active( const active_mask_t &active ) {
    m_warp_active_mask = active;
    if( m_isatomic ) {
	for( unsigned i=0; i < m_config->warp_size; i++ ) {
	    if( !m_warp_active_mask.test(i) ) {
		m_per_scalar_thread[i].callback.function = NULL;
		m_per_scalar_thread[i].callback.instruction = NULL;
		m_per_scalar_thread[i].callback.thread = NULL;
	    }
	}
    }
}

void warp_inst_t::do_atomic(bool forceDo) {
    do_atomic( m_warp_active_mask,forceDo );
}


void warp_inst_t::do_atomic( const active_mask_t& access_mask,bool forceDo ) {
    assert( m_isatomic && (!m_empty||forceDo) );
    for( unsigned i=0; i < m_config->warp_size; i++ )
    {
	if( access_mask.test(i) )
	{
	    dram_callback_t &cb = m_per_scalar_thread[i].callback;
	    if( cb.thread )
		cb.function(cb.instruction, cb.thread);
	}
    }
}

void warp_inst_t::broadcast_barrier_reduction(const active_mask_t& access_mask)
{
    for( unsigned i=0; i < m_config->warp_size; i++ )
    {
	if( access_mask.test(i) )
	{
	    dram_callback_t &cb = m_per_scalar_thread[i].callback;
	    if( cb.thread ){
		cb.function(cb.instruction, cb.thread);
	    }
	}
    }
}

void warp_inst_t::generate_mem_accesses()
{
    //fprintf(stdout, "begin generate_mem_accesses\n");
    if( empty() || op == MEMORY_BARRIER_OP || m_mem_accesses_created ) 
	return;
    if ( !((op == LOAD_OP) || (op == STORE_OP)) )
	return; 
    if( m_warp_active_mask.count() == 0 ) 
	return; // predicated off

    const size_t starting_queue_size = m_accessq.size();

    assert( is_load() || is_store() );
    assert( m_per_scalar_thread_valid ); // need address information per thread

    bool is_write = is_store();

    mem_access_type access_type;
    switch (space.get_type()) {
	case const_space:
	case param_space_kernel: 
	    access_type = CONST_ACC_R; 
	    break;
	case tex_space: 
	    access_type = TEXTURE_ACC_R;   
	    break;
	case child_param_space: 
	case global_space:       
	    access_type = is_write? GLOBAL_ACC_W: GLOBAL_ACC_R;   
	    break;
	case local_space:
	case param_space_local:  
	    access_type = is_write? LOCAL_ACC_W: LOCAL_ACC_R;   
	    break;
	case shared_space: break;
//	case child_param_space: 
//			   fprintf(stdout, "leave generate_mem_accesses early\n");
//			   return;
	default: assert(0); break; 
    }

    // Calculate memory accesses generated by this warp
    new_addr_type cache_block_size = 0; // in bytes 

    switch( space.get_type() ) {
	case shared_space: {
			       unsigned subwarp_size = m_config->warp_size / m_config->mem_warp_parts;
			       unsigned total_accesses=0;
			       for( unsigned subwarp=0; subwarp <  m_config->mem_warp_parts; subwarp++ ) {

				   // data structures used per part warp 
				   std::map<unsigned,std::map<new_addr_type,unsigned> > bank_accs; // bank -> word address -> access count

				   // step 1: compute accesses to words in banks
				   for( unsigned thread=subwarp*subwarp_size; thread < (subwarp+1)*subwarp_size; thread++ ) {
				       if( !active(thread) ) 
					   continue;
				       new_addr_type addr = m_per_scalar_thread[thread].memreqaddr[0];
				       //FIXME: deferred allocation of shared memory should not accumulate across kernel launches
				       //assert( addr < m_config->gpgpu_shmem_size ); 
				       unsigned bank = m_config->shmem_bank_func(addr);
				       new_addr_type word = line_size_based_tag_func(addr,m_config->WORD_SIZE);
				       bank_accs[bank][word]++;
				   }

				   if (m_config->shmem_limited_broadcast) {
				       // step 2: look for and select a broadcast bank/word if one occurs
				       bool broadcast_detected = false;
				       new_addr_type broadcast_word=(new_addr_type)-1;
				       unsigned broadcast_bank=(unsigned)-1;
				       std::map<unsigned,std::map<new_addr_type,unsigned> >::iterator b;
				       for( b=bank_accs.begin(); b != bank_accs.end(); b++ ) {
					   unsigned bank = b->first;
					   std::map<new_addr_type,unsigned> &access_set = b->second;
					   std::map<new_addr_type,unsigned>::iterator w;
					   for( w=access_set.begin(); w != access_set.end(); ++w ) {
					       if( w->second > 1 ) {
						   // found a broadcast
						   broadcast_detected=true;
						   broadcast_bank=bank;
						   broadcast_word=w->first;
						   break;
					       }
					   }
					   if( broadcast_detected ) 
					       break;
				       }

				       // step 3: figure out max bank accesses performed, taking account of broadcast case
				       unsigned max_bank_accesses=0;
				       for( b=bank_accs.begin(); b != bank_accs.end(); b++ ) {
					   unsigned bank_accesses=0;
					   std::map<new_addr_type,unsigned> &access_set = b->second;
					   std::map<new_addr_type,unsigned>::iterator w;
					   for( w=access_set.begin(); w != access_set.end(); ++w ) 
					       bank_accesses += w->second;
					   if( broadcast_detected && broadcast_bank == b->first ) {
					       for( w=access_set.begin(); w != access_set.end(); ++w ) {
						   if( w->first == broadcast_word ) {
						       unsigned n = w->second;
						       assert(n > 1); // or this wasn't a broadcast
						       assert(bank_accesses >= (n-1));
						       bank_accesses -= (n-1);
						       break;
						   }
					       }
					   }
					   if( bank_accesses > max_bank_accesses ) 
					       max_bank_accesses = bank_accesses;
				       }

				       // step 4: accumulate
				       total_accesses+= max_bank_accesses;
				   } else {
				       // step 2: look for the bank with the maximum number of access to different words 
				       unsigned max_bank_accesses=0;
				       std::map<unsigned,std::map<new_addr_type,unsigned> >::iterator b;
				       for( b=bank_accs.begin(); b != bank_accs.end(); b++ ) {
					   max_bank_accesses = std::max(max_bank_accesses, (unsigned)b->second.size());
				       }

				       // step 3: accumulate
				       total_accesses+= max_bank_accesses;
				   }
			       }
			       assert( total_accesses > 0 && total_accesses <= m_config->warp_size );
			       cycles = total_accesses; // shared memory conflicts modeled as larger initiation interval 
			       ptx_file_line_stats_add_smem_bank_conflict( pc, total_accesses );
			       break;
			   }

	case tex_space: 
			   cache_block_size = m_config->gpgpu_cache_texl1_linesize;
			   break;

	case param_space_kernel:
//			   extern bool g_child_param_buffer_compaction;
			   /* Po-Han 170602 Redirect device-launched kernel parameter loads from constant cache to unified L1 cache */
			   extern bool g_param_acc_unified_L1;
			   if( g_param_acc_unified_L1 ){
			       if (m_per_scalar_thread[0].memreqaddr[0] >= CHILD_PARAM_START && m_per_scalar_thread[0].memreqaddr[0] < CHILD_PARAM_END ){
				   space.set_type(global_space);
				   access_type = GLOBAL_ACC_R;
				   if( m_config->gpgpu_coalesce_arch == 13 ){
				       if(isatomic())
					   memory_coalescing_arch_13_atomic(is_write, access_type);
				       else
					   memory_coalescing_arch_13(is_write, access_type);
				   } else abort();
				   break;
			       }
			   }
			   if (m_per_scalar_thread[0].memreqaddr[0] == 0xFEEBDAED){ //consntant accesses are bypassed
//			       printf("Bypass parameter loads\n");
			       return;
			   } /*else if (g_child_param_buffer_compaction){
			       if( m_per_scalar_thread[0].memreqaddr[0] >= CHILD_PARAM_START && m_per_scalar_thread[0].memreqaddr[0] < CHILD_PARAM_END){
				   space.set_type(global_space);
				   memory_coalescing_arch_13(false, GLOBAL_ACC_R); //access data cache for child kernel parameters;
			       }
			   }*/ else {
			       cache_block_size = m_config->gpgpu_cache_constl1_linesize;
			   }
			   break;
	case const_space:  //case param_space_kernel:
			   cache_block_size = m_config->gpgpu_cache_constl1_linesize; 
			   break;

	case global_space: case local_space: case param_space_local:
	case child_param_space:
			   if( m_config->gpgpu_coalesce_arch == 13 ) {
			       if(isatomic())
				   memory_coalescing_arch_13_atomic(is_write, access_type);
			       else
				   memory_coalescing_arch_13(is_write, access_type);
			   } else abort();

			   break;

	default:
			   abort();
    }

    bool global_constant_sharing_bypass;
    std::map<new_addr_type,active_mask_t> accesses; // block address -> set of thread offsets in warp
    if( cache_block_size ) {
	assert( m_accessq.empty() );
	mem_access_byte_mask_t byte_mask; 
//	std::map<new_addr_type,active_mask_t> accesses; // block address -> set of thread offsets in warp
	std::map<new_addr_type,active_mask_t>::iterator a;
	for( unsigned thread=0; thread < m_config->warp_size; thread++ ) {
	    if( !active(thread) ) 
		continue;
	    new_addr_type addr = m_per_scalar_thread[thread].memreqaddr[0];
#if 0
	    global_constant_sharing_bypass = false;
	    if(space.get_type() == param_space_kernel){
//		    printf("Kernel parameter access at address 0x%llx\n", addr);
		extern int g_global_constant_pointer_sharing;
		extern bool g_child_param_buffer_compaction;
		if( !g_child_param_buffer_compaction ){
		    if( g_global_constant_pointer_sharing != 4 )
			global_constant_sharing_bypass = check_global_constant_sharing_bypass(addr, pc);
		    if( global_constant_sharing_bypass ) {
			extern int g_child_kernel_param_bypass_cnt[2];
			g_child_kernel_param_bypass_cnt[0]++;
		    }
		}
	    }
#endif
//	    if(!global_constant_sharing_bypass){
		unsigned block_address = line_size_based_tag_func(addr,cache_block_size);
		accesses[block_address].set(thread);
		unsigned idx = addr-block_address; 
		for( unsigned i=0; i < data_size; i++ ) 
		    byte_mask.set(idx+i);
//	    }
	}
	for( a=accesses.begin(); a != accesses.end(); ++a ) 
	    m_accessq.push_back( mem_access_t(access_type,a->first,cache_block_size,is_write,a->second,byte_mask) );
    }

    if ( space.get_type() == global_space ) {
	ptx_file_line_stats_add_uncoalesced_gmem( pc, m_accessq.size() - starting_queue_size );
    }

    /* may actually not genearting any cache accesses because of global constant parameter sharing */
    if(accesses.size() != 0)
	m_mem_accesses_created=true;
}

void warp_inst_t::memory_coalescing_arch_13( bool is_write, mem_access_type access_type )
{
    // see the CUDA manual where it discusses coalescing rules before reading this
    unsigned segment_size = 0;
    unsigned warp_parts = m_config->mem_warp_parts;
    switch( data_size ) {
	case 1: segment_size = 32; break;
	case 2: segment_size = 64; break;
	case 4: case 8: case 16: segment_size = 128; break;
    }
    unsigned subwarp_size = m_config->warp_size / warp_parts;

    bool global_constant_sharing_bypass;

    for( unsigned subwarp=0; subwarp <  warp_parts; subwarp++ ) {
	std::map<new_addr_type,transaction_info> subwarp_transactions;

	// step 1: find all transactions generated by this subwarp
	for( unsigned thread=subwarp*subwarp_size; thread<subwarp_size*(subwarp+1); thread++ ) {
	    if( !active(thread) )
		continue;

	    unsigned data_size_coales = data_size;
	    unsigned num_accesses = 1;

	    // bddream TEST: parameter writes should not split into 4B chunks
	    if( space.get_type() == local_space || space.get_type() == param_space_local /*|| space.get_type() == child_param_space*/  ) {
		// Local memory accesses >4B were split into 4B chunks
		if(data_size >= 4) {
		    data_size_coales = 4;
		    num_accesses = data_size/4;
		}
		// Otherwise keep the same data_size for sub-4B access to local memory
	    }


	    assert(num_accesses <= MAX_ACCESSES_PER_INSN_PER_THREAD);

	    for(unsigned access=0; access<num_accesses; access++) {
		global_constant_sharing_bypass = false;
		new_addr_type addr = m_per_scalar_thread[thread].memreqaddr[access];
		extern int g_global_constant_pointer_sharing;
		if( space.get_type() == child_param_space ) {
//		    if( g_global_constant_pointer_sharing != 3 ){
			global_constant_sharing_bypass = check_global_constant_sharing_bypass(addr, pc);
//		    }
		}
//		if( global_constant_sharing_bypass ) {
//		    extern int g_child_kernel_param_bypass_cnt[2];
//		    g_child_kernel_param_bypass_cnt[1]++;
//		}
		if( !global_constant_sharing_bypass ){
		    unsigned block_address = line_size_based_tag_func(addr,segment_size);
		    unsigned chunk = (addr&127)/32; // which 32-byte chunk within in a 128-byte chunk does this thread access?
		    transaction_info &info = subwarp_transactions[block_address];

		    // can only write to one segment
		    //		fprintf(stdout, "space = %d block_address = %u, addr = %llx, segment_size = %u\n", space.get_type(), block_address, addr, segment_size);
		    //		assert(block_address == line_size_based_tag_func(addr+data_size_coales-1,segment_size));

		    info.chunks.set(chunk);
		    info.active.set(thread);
		    unsigned idx = (addr&127);
		    for( unsigned i=0; i < data_size_coales; i++ )
			info.bytes.set(idx+i);
		}
	    }
	}

	// step 2: reduce each transaction size, if possible
	std::map< new_addr_type, transaction_info >::iterator t;
	for( t=subwarp_transactions.begin(); t !=subwarp_transactions.end(); t++ ) {
	    new_addr_type addr = t->first;
	    const transaction_info &info = t->second;

	    memory_coalescing_arch_13_reduce_and_send(is_write, access_type, info, addr, segment_size);

	}
    }
}

void warp_inst_t::memory_coalescing_arch_13_atomic( bool is_write, mem_access_type access_type )
{

    assert(space.get_type() == global_space); // Atomics allowed only for global memory

    // see the CUDA manual where it discusses coalescing rules before reading this
    unsigned segment_size = 0;
    unsigned warp_parts = 2;
    switch( data_size ) {
	case 1: segment_size = 32; break;
	case 2: segment_size = 64; break;
	case 4: case 8: case 16: segment_size = 128; break;
    }
    unsigned subwarp_size = m_config->warp_size / warp_parts;

    for( unsigned subwarp=0; subwarp <  warp_parts; subwarp++ ) {
	std::map<new_addr_type,std::list<transaction_info> > subwarp_transactions; // each block addr maps to a list of transactions

	// step 1: find all transactions generated by this subwarp
	for( unsigned thread=subwarp*subwarp_size; thread<subwarp_size*(subwarp+1); thread++ ) {
	    if( !active(thread) )
		continue;

	    new_addr_type addr = m_per_scalar_thread[thread].memreqaddr[0];
	    unsigned block_address = line_size_based_tag_func(addr,segment_size);
	    unsigned chunk = (addr&127)/32; // which 32-byte chunk within in a 128-byte chunk does this thread access?

	    // can only write to one segment
	    assert(block_address == line_size_based_tag_func(addr+data_size-1,segment_size));

	    // Find a transaction that does not conflict with this thread's accesses
	    bool new_transaction = true;
	    std::list<transaction_info>::iterator it;
	    transaction_info* info;
	    for(it=subwarp_transactions[block_address].begin(); it!=subwarp_transactions[block_address].end(); it++) {
		unsigned idx = (addr&127);
		if(not it->test_bytes(idx,idx+data_size-1)) {
		    new_transaction = false;
		    info = &(*it);
		    break;
		}
	    }
	    if(new_transaction) {
		// Need a new transaction
		subwarp_transactions[block_address].push_back(transaction_info());
		info = &subwarp_transactions[block_address].back();
	    }
	    assert(info);

	    info->chunks.set(chunk);
	    info->active.set(thread);
	    unsigned idx = (addr&127);
	    for( unsigned i=0; i < data_size; i++ ) {
		assert(!info->bytes.test(idx+i));
		info->bytes.set(idx+i);
	    }
	}

	// step 2: reduce each transaction size, if possible
	std::map< new_addr_type, std::list<transaction_info> >::iterator t_list;
	for( t_list=subwarp_transactions.begin(); t_list !=subwarp_transactions.end(); t_list++ ) {
	    // For each block addr
	    new_addr_type addr = t_list->first;
	    const std::list<transaction_info>& transaction_list = t_list->second;

	    std::list<transaction_info>::const_iterator t;
	    for(t=transaction_list.begin(); t!=transaction_list.end(); t++) {
		// For each transaction
		const transaction_info &info = *t;
		memory_coalescing_arch_13_reduce_and_send(is_write, access_type, info, addr, segment_size);
	    }
	}
    }
}

void warp_inst_t::memory_coalescing_arch_13_reduce_and_send( bool is_write, mem_access_type access_type, const transaction_info &info, new_addr_type addr, unsigned segment_size )
{
    assert( (addr & (segment_size-1)) == 0 );

    const std::bitset<4> &q = info.chunks;
    assert( q.count() >= 1 );
    std::bitset<2> h; // halves (used to check if 64 byte segment can be compressed into a single 32 byte segment)

    unsigned size=segment_size;
    if( segment_size == 128 ) {
	bool lower_half_used = q[0] || q[1];
	bool upper_half_used = q[2] || q[3];
	if( lower_half_used && !upper_half_used ) {
	    // only lower 64 bytes used
	    size = 64;
	    if(q[0]) h.set(0);
	    if(q[1]) h.set(1);
	} else if ( (!lower_half_used) && upper_half_used ) {
	    // only upper 64 bytes used
	    addr = addr+64;
	    size = 64;
	    if(q[2]) h.set(0);
	    if(q[3]) h.set(1);
	} else {
	    assert(lower_half_used && upper_half_used);
	}
    } else if( segment_size == 64 ) {
	// need to set halves
	if( (addr % 128) == 0 ) {
	    if(q[0]) h.set(0);
	    if(q[1]) h.set(1);
	} else {
	    assert( (addr % 128) == 64 );
	    if(q[2]) h.set(0);
	    if(q[3]) h.set(1);
	}
    }
    if( size == 64 ) {
	bool lower_half_used = h[0];
	bool upper_half_used = h[1];
	if( lower_half_used && !upper_half_used ) {
	    size = 32;
	} else if ( (!lower_half_used) && upper_half_used ) {
	    addr = addr+32;
	    size = 32;
	} else {
	    assert(lower_half_used && upper_half_used);
	}
    }
    m_accessq.push_back( mem_access_t(access_type,addr,size,is_write,info.active,info.bytes) );
    extern unsigned long long g_total_ld_cache_line, g_total_st_cache_line;
    if(is_write) g_total_st_cache_line++;
    else g_total_ld_cache_line++;
}

/* Checking is a child kernel parameter access can bypass since it is a global constant parameter */
bool warp_inst_t::check_global_constant_sharing_bypass( new_addr_type addr, unsigned pc )
{
//    extern bool g_child_param_buffer_compaction;
    extern int g_global_constant_pointer_sharing;
//    extern int g_child_parameter_buffer_alignment;
    extern application_id g_app_name;
//    extern int global_constant_offset[12][6];
    extern unsigned global_constant_pc[12][12];
    if(addr >= CHILD_PARAM_START && addr < CHILD_PARAM_END){
	if(g_global_constant_pointer_sharing > 0){
	    if(g_global_constant_pointer_sharing == 2){ // always bypass
//		printf("CPB: always bypassing\n");
		return true;
	    } else if (g_global_constant_pointer_sharing == 1){ 
//		if(g_child_param_buffer_compaction){
		    //check PC
		if(g_app_name == 1 || g_app_name == 7 || g_app_name == 8 ){
		    if ( (pc >= global_constant_pc[g_app_name][0] && pc <= global_constant_pc[g_app_name][1]) ||
			 (pc >= global_constant_pc[g_app_name][2] && pc <= global_constant_pc[g_app_name][3]) )
			return true;
		} else {
		    for( int i = 0; i < 12; i++ ){
			if( global_constant_pc[g_app_name][i] == pc ){
			    //			    printf("CPB: pc 0x%04X app_name %d gc_num %d\n", pc, g_app_name, i);
			    return true;
			}
		    }
		}
		//		} else {
		    // check address
//		    unsigned int offset = addr % g_child_parameter_buffer_alignment;
//		    for( int i = 0; i < 6; i++ ){
//			if( global_constant_offset[g_app_name][i] != -1 && global_constant_offset[g_app_name][i] == offset ){
			    //			printf("CPB: addr %llx offset %d app_name %d gc_num %d\n", addr, offset, g_app_name, i);
//			    return true;
//			}
//		    }
//		}
	    }
	}
    }
    return false;
}

void warp_inst_t::completed( unsigned long long cycle ) const 
{
    unsigned long long latency = cycle - issue_cycle; 
    assert(latency <= cycle); // underflow detection 
    ptx_file_line_stats_add_latency(pc, latency * active_count());  
}

//Jin: kernel launch latency and overhead
unsigned g_kernel_launch_latency;
extern signed long long g_total_param_size;

unsigned kernel_info_t::m_next_uid = 1;

kernel_info_t::kernel_info_t( dim3 gridDim, dim3 blockDim, class function_info *entry )
{
    m_kernel_entry=entry;
    m_grid_dim=gridDim;
    m_block_dim=blockDim;
    m_next_cta.x=0;
    m_next_cta.y=0;
    m_next_cta.z=0;
    m_next_tid=m_next_cta;
    m_num_cores_running=0;
    m_uid = m_next_uid++;
    m_param_mem = new memory_space_impl<256>("param", 256);

    //Jin: parent and child kernel management for CDP
    m_parent_kernel = NULL;

    //Jin: aggregated cta management
    m_next_agg_group_id = -1; //start from native ctas
    m_total_agg_group_id = 0;
    m_total_num_agg_blocks = 0;

    //Jin: launch latency management
    m_launch_latency = g_kernel_launch_latency;

    //Po-Han: DCC
    is_child = false;
    param_entry_cnt = -1;
    parent_child_dependency = false;
    end_cycle = 0;
    m_param_mem_base = 0;
    // dekline
    // For kernel block metadata
    block_state = new block_info[m_grid_dim.x * m_grid_dim.y * m_grid_dim.z];
    unsigned i = 0;

    while (!no_more_block_to_run()) {
	dim3 id = get_next_cta_id();
	block_state[i].block_id.x = id.x;
	block_state[i].block_id.y = id.y;
	block_state[i].block_id.z = id.z;
	block_state[i].issued = 0;
	block_state[i].done = 0;
	block_state[i].switched = 0;
	block_state[i].preempted = 0;
	block_state[i].reissue = 0;
	block_state[i].time_stamp_switching = 0;
	block_state[i].time_stamp_switching_issue = 0;
	block_state[i].thread.set();
	block_state[i].devsynced = false;

//	for (unsigned j=0; j<threads_per_cta(); j++)
//	    block_state[i].thread.reset(j);

	increment_block_id();
	i++;
    }

    m_next_cta.x = 0;
    m_next_cta.y = 0;
    m_next_cta.z = 0;
    //switched_done = 0;
    //
    start_cycle = 0;
    extra_metadata_load_latency = false;
    next_dispatchable_cycle = 0;
    metadata_count = 1;
    unissued_agg_groups = 0;
}

// bddream DCC
// reset block state for child kernel
void kernel_info_t::reset_block_state()
{
   delete[] block_state;
   block_state = new block_info[m_grid_dim.x * m_grid_dim.y * m_grid_dim.z];
   unsigned i = 0;
   m_next_cta.x = 0;
   m_next_cta.y = 0;
   m_next_cta.z = 0;

   while (!no_more_block_to_run()) {
      dim3 id = get_next_cta_id();
      block_state[i].block_id.x = id.x;
      block_state[i].block_id.y = id.y;
      block_state[i].block_id.z = id.z;
      block_state[i].issued = 0;
      block_state[i].done = 0;
      block_state[i].switched = 0;
      block_state[i].preempted = 0;
      block_state[i].reissue = 0;
      block_state[i].time_stamp_switching = 0;
      block_state[i].time_stamp_switching_issue = 0;
      block_state[i].thread.set();
      block_state[i].devsynced = false;

//      for (unsigned j=0; j<threads_per_cta(); j++)
//       block_state[i].thread.reset(j);

      increment_block_id();
      i++;
   }

   m_next_cta.x = 0;
   m_next_cta.y = 0;
   m_next_cta.z = 0;

   //switched_done = 0;
   return;
}

// dekline
// check if all switched out CTA are finished
#if 0
bool kernel_info_t::switched_done() const
{
	bool done = true;
    for ( unsigned i=0; i<num_blocks()/*(unsigned)m_grid_dim.x * m_grid_dim.y * m_grid_dim.z*/; i++ ) {
	if (block_state[i].preempted/*switched*/ && !block_state[i].done){
	    done =  false;
	    break;
	}
    }

    return done;
}
#endif

// dekline
// find global cta idx for the block
unsigned kernel_info_t::find_block_idx( dim3 block_id )
{
    return block_id.x + block_id.y * m_grid_dim.x + block_id.z * m_grid_dim.x * m_grid_dim.y;
}

kernel_info_t::~kernel_info_t()
{
    assert( m_active_threads.empty() );
    destroy_cta_streams();
    destroy_agg_block_groups();

    extern bool g_dyn_child_thread_consolidation;
    if( g_dyn_child_thread_consolidation ){
	extern bool *g_kernel_queue_entry_empty;
	extern unsigned int g_kernel_queue_entry_used, g_kernel_queue_entry_running;
	std::map<unsigned int, int>::iterator it;
	for( it = m_kernel_queue_entry_map.begin(); it != m_kernel_queue_entry_map.end(); it++){
	    if(it->second != -1){
		assert(g_kernel_queue_entry_empty[it->second] == false);
		g_kernel_queue_entry_empty[it->second] = true;
		if( g_kernel_queue_entry_used ) g_kernel_queue_entry_used--;
		if( g_kernel_queue_entry_running) g_kernel_queue_entry_running--;
	    } else {
		extern unsigned long long total_num_offchip_metadata;
		if( total_num_offchip_metadata > 0 )
		    total_num_offchip_metadata--;
		printf("DKC: reclaim an off-chip kernel metadata, %llu remaining\n", total_num_offchip_metadata);
	    }
	    printf("TDQ, %llu, %d, %d, D\n", gpu_sim_cycle+gpu_tot_sim_cycle, it->second, g_kernel_queue_entry_used);
	}
	printf("DKC: Consolidated kernel ends, MDB usage %u, MDB running %u\n", g_kernel_queue_entry_used, g_kernel_queue_entry_running);
//	delete m_kernel_queue_entry_map;
    }
    delete m_param_mem;
    // dekline
    delete[] block_state;
}

std::string kernel_info_t::name() const
{
    return m_kernel_entry->get_name();
}

//Jin: parent and child kernel management for CDP
void kernel_info_t::set_parent(kernel_info_t * parent, 
	int parent_agg_group_id,
	dim3 parent_ctaid, dim3 parent_tid, unsigned parent_block_idx, unsigned parent_thread_idx) {
    m_parent_kernel = parent;
    m_parent_agg_group_id = parent_agg_group_id;
    m_parent_ctaid = parent_ctaid;
    m_parent_block_idx = parent_block_idx;
    m_parent_tid = parent_tid;
    m_parent_thread_idx = parent_thread_idx;
    parent->set_child(this);
}

//Po-Han: DCC: add a new parent thread for the child kernel
void kernel_info_t::add_parent(kernel_info_t * parent, ptx_thread_info * thread) {
    if(!m_parent_kernel){ 
	m_parent_kernel = parent;
	m_parent_agg_group_id = thread->get_agg_group_id();
	m_parent_ctaid = thread->get_ctaid();
	m_parent_tid = thread->get_tid();
	parent->set_child(this);
	fprintf(stdout, "DCC: adding parent-child relation between %llx (B%u, T%u) and %llx, %llx now has %d childs.\n", parent, m_parent_ctaid.x, m_parent_tid.x, this, parent, parent->get_child_count());
    }
    m_parent_threads.push_back(thread);
    //    parent->set_child(this);
    //    fprintf(stdout, "DCC: adding parent-child relation between %llx and %llx, %llx now has %d childs.\n", parent, this, parent, parent->get_child_count());
}

void kernel_info_t::set_child(kernel_info_t * child) {
    m_child_kernels.push_back(child);
}

void kernel_info_t::remove_child(kernel_info_t * child) {
    assert(std::find(m_child_kernels.begin(), m_child_kernels.end(), child)
	    != m_child_kernels.end());
    m_child_kernels.remove(child);
}

bool kernel_info_t::is_finished() {
    if(done() && children_all_finished() )
	return true;
    else
	return false;
}

bool kernel_info_t::children_all_finished() {
    if(!m_child_kernels.empty())
	return false;

    return true;
}

void kernel_info_t::notify_parent_finished() {
    unsigned tmp_parent_block_idx, tmp_parent_thread_idx;
    if(m_parent_kernel) {
	if(name().find("kmeansPoint_CdpKernel") != std::string::npos || 
		name().find("calc_pi_CdpKernel") != std::string::npos ||
		name().find("update_bias_CdpKernel") != std::string::npos ||
		name().find("find1_CdpKernel") != std::string::npos ||
		name().find("find2_CdpKernel") != std::string::npos ||
		name().find("relabelUnrollKernel") != std::string::npos ){
	    if(g_agg_blocks_support || g_dyn_child_thread_consolidation){ //DTBL or DKC
		char mech[5], sche[5];
		extern bool g_child_aware_smk_scheduling;
		if(g_dyn_child_thread_consolidation) sprintf(mech, "DKC");
		else sprintf(mech, "DTBL");
		if(g_child_aware_smk_scheduling) sprintf(sche, "-DPS");
		else sprintf(sche, "");

		std::list<ptx_thread_info *>::iterator it;
		for(it=m_parent_threads.begin(); it!=m_parent_threads.end(); it++){
		    tmp_parent_block_idx = (*it)->get_block_idx();
		    tmp_parent_thread_idx = (*it)->get_thread_idx();
		    m_parent_kernel->block_state[tmp_parent_block_idx].thread.set(tmp_parent_thread_idx);
		    fprintf(stdout, "%s: [%d, %d, %d] -- child kernel finished\n", mech, m_parent_kernel->get_uid(), tmp_parent_block_idx, tmp_parent_thread_idx);
		    if(g_child_aware_smk_scheduling){ //DPS
			if(m_parent_kernel->block_state[tmp_parent_block_idx].thread.all()){
			    if(m_parent_kernel->block_state[tmp_parent_block_idx].switched == 1){
				m_parent_kernel->block_state[tmp_parent_block_idx].switched = 0;
				fprintf(stdout, "%s%s: [%d, %d] -- all child kernels finished\n", mech, sche, m_parent_kernel->get_uid(), tmp_parent_block_idx);
			    }
			}
		    } else { //nonDPS
			if(m_parent_kernel->block_state[tmp_parent_block_idx].thread.all()){
			    if(m_parent_kernel->block_state[tmp_parent_block_idx].preempted){ //preempted --> re-issue it
				m_parent_kernel->block_state[tmp_parent_block_idx].reissue = 1;
				fprintf(stdout, "%s%s: [%d, %d] -- all child kernels finished, parent block preempteded --> re-issue it\n", mech, sche, m_parent_kernel->get_uid(), tmp_parent_block_idx);
			    }else{ //not yet preempted --> mark-off the switch bit
				if(m_parent_kernel->block_state[tmp_parent_block_idx].switched == 1){
				    m_parent_kernel->block_state[tmp_parent_block_idx].switched = 0;
				    m_parent_kernel->switching_list.remove(tmp_parent_block_idx);
				    m_parent_kernel->preswitch_list.remove(tmp_parent_block_idx);
				    fprintf(stdout, "%s%s: [%d, %d] -- all child kernels finished, parent block not-yet preempted --> resume it\n", mech, sche, m_parent_kernel->get_uid(), tmp_parent_block_idx);
				}
			    }
			}
		    }
		}
	    } else { //CDP
		m_parent_kernel->block_state[m_parent_block_idx].thread.set(m_parent_thread_idx);
		tmp_parent_block_idx = m_parent_block_idx;
		fprintf(stdout, "CDP: [%d, %d, %d] -- child kernel finished\n", m_parent_kernel->get_uid(), m_parent_block_idx, m_parent_thread_idx);
		if(m_parent_kernel->block_state[m_parent_block_idx].thread.all()){
		    if(m_parent_kernel->block_state[m_parent_block_idx].preempted){ //preempted --> re-issue it
			m_parent_kernel->block_state[m_parent_block_idx].reissue = 1;
			fprintf(stdout, "CDP: [%d, %d] -- all child kernels finished, parent block preempteded --> re-issue it\n", m_parent_kernel->get_uid(), m_parent_block_idx);
		    }else{ //not yet preempted --> mark-off the switch bit
			if(m_parent_kernel->block_state[m_parent_block_idx].switched == 1){
			    m_parent_kernel->block_state[m_parent_block_idx].switched = 0;
			    m_parent_kernel->switching_list.remove(m_parent_block_idx);
			    m_parent_kernel->preswitch_list.remove(m_parent_block_idx);
			    fprintf(stdout, "CDP: [%d, %d] -- all child kernels finished, parent block not-yet preempted --> resume it\n", m_parent_kernel->get_uid(), m_parent_block_idx);
			}
		    }
		}
	    }
	}
	extern int g_child_parameter_buffer_alignment;
	extern bool g_global_constant_pointer_sharing;
	extern int per_kernel_param_usage[12];
	extern application_id g_app_name;
	int param_buf_size;
	
	if(g_global_constant_pointer_sharing == 1)
	    param_buf_size = per_kernel_param_usage[g_app_name];
	else
	    param_buf_size = m_kernel_entry->get_args_aligned_size();

	if( g_dyn_child_thread_consolidation ){
	    g_total_param_size -= metadata_count * ((param_buf_size + g_child_parameter_buffer_alignment - 1) / g_child_parameter_buffer_alignment * g_child_parameter_buffer_alignment);
	} else {
	    g_total_param_size -= ((param_buf_size + g_child_parameter_buffer_alignment - 1) / g_child_parameter_buffer_alignment * g_child_parameter_buffer_alignment);
	}
	if(g_total_param_size < 0) g_total_param_size = 0;
	m_parent_kernel->remove_child(this);
	fprintf(stdout, "Remove child-kernel %d from parent %d, %d now has %d childs.\n", this->get_uid(), m_parent_kernel->get_uid(), m_parent_kernel->get_uid(), m_parent_kernel->get_child_count());
	g_stream_manager->register_finished_kernel(m_parent_kernel->get_uid());
	/* TODO: Po-Han DCC
	 * 1) set the corresponding parent threads as child-finished
	 * 2) if a whole parent block are child-finished, context switch it back (cdp) or resume it (DCC
	 */

    }
}

CUstream_st * kernel_info_t::create_stream_cta(int agg_group_id, dim3 ctaid) {
	   assert(get_default_stream_cta(agg_group_id, ctaid));
	   //    get_default_stream_cta(agg_group_id, ctaid);
	   CUstream_st * stream = new CUstream_st();
    g_stream_manager->add_stream(stream);
    agg_block_id_t agg_block_id(agg_group_id, ctaid);
    assert(m_cta_streams.find(agg_block_id) != m_cta_streams.end());
    assert(m_cta_streams[agg_block_id].size() >= 1); //must have default stream
    m_cta_streams[agg_block_id].push_back(stream);
//    fprintf(stdout, "STREAMMANAGER, %d, %u, %u, %u, add, %0llx, %d\n", agg_group_id, ctaid.x, ctaid.y, ctaid.z, stream, g_stream_manager->stream_count()); fflush(stdout);
/*    fprintf(stdout, "STREAMMANAGER, %d, %u, %u, %u, add, %0llx, %d\nm_cta_streams:\n", agg_group_id, ctaid.x, ctaid.y, ctaid.z, stream, g_stream_manager->stream_count()); 
    for(auto s = m_cta_streams.begin(); s != m_cta_streams.end(); s++ ){
	    fprintf(stdout, "[%d, (%u, %u, %u)] ->", s->first.first, s->first.second.x, s->first.second.y, s->first.second.z);
	    if(s->second.size() != 0){
		    for(auto ss = s->second.begin(); ss != s->second.end(); ss++){
			    fprintf(stdout, " (%d, %llx)", (*ss)->get_uid(), *ss);
		    }
	    }
	    fprintf(stdout, "\n");
    }
    fprintf(stdout, "\n");*/

    return stream;
}

void kernel_info_t::delete_stream_cta(int agg_group_id, dim3 ctaid, CUstream_st* stream) {
//    assert(get_default_stream_cta(agg_group_id, ctaid));
//    CUstream_st * stream = new CUstream_st();
    g_stream_manager->destroy_stream(stream);
//    fprintf(stdout, "STREAMMANAGER, %d, %u, %u, %u, del, %0llx, %d\n", agg_group_id, ctaid.x, ctaid.y, ctaid.z, stream, g_stream_manager->stream_count()); fflush(stdout);

    agg_block_id_t agg_block_id(agg_group_id, ctaid);
//    assert(m_cta_streams.find(agg_block_id) != m_cta_streams.end());
//    assert(m_cta_streams[agg_block_id].size() >= 1); //must have default stream
    for(auto s = m_cta_streams[agg_block_id].begin(); s != m_cta_streams[agg_block_id].end(); s++){
       if(*s == stream){
          m_cta_streams[agg_block_id].erase(s);
          break;
       }
    }
}

CUstream_st * kernel_info_t::get_default_stream_cta(int agg_group_id, dim3 ctaid) {

    agg_block_id_t agg_block_id(agg_group_id, ctaid);
    if(m_cta_streams.find(agg_block_id) != m_cta_streams.end()) {
	assert(m_cta_streams[agg_block_id].size() >= 1); //already created, must have default stream
	return *(m_cta_streams[agg_block_id].begin());
    }
    else {
	m_cta_streams[agg_block_id] = std::list<CUstream_st *>();
	CUstream_st * stream = new CUstream_st();
	g_stream_manager->add_stream(stream);
        m_cta_streams[agg_block_id].push_back(stream);
/*        fprintf(stdout, "STREAMMANAGER, %d, %u, %u, %u, add, %0llx, %d\nm_cta_streams:\n", agg_group_id, ctaid.x, ctaid.y, ctaid.z, stream, g_stream_manager->stream_count()); 
	for(auto s = m_cta_streams.begin(); s != m_cta_streams.end(); s++ ){
		fprintf(stdout, "[%d, (%u, %u, %u)] ->", s->first.first, s->first.second.x, s->first.second.y, s->first.second.z);
		if(s->second.size() != 0){
			for(auto ss = s->second.begin(); ss != s->second.end(); ss++){
				fprintf(stdout, " (%d, %llx)", (*ss)->get_uid(), *ss);
			}
		}
		fprintf(stdout, "\n");
	}
	fprintf(stdout, "\n");
	fflush(stdout);*/
	return stream;
    }
}

bool kernel_info_t::cta_has_stream(int agg_group_id, dim3 ctaid, CUstream_st* stream) {
    agg_block_id_t agg_block_id(agg_group_id, ctaid);
    if(m_cta_streams.find(agg_block_id) == m_cta_streams.end())
	return false;

    std::list<CUstream_st *> &stream_list = m_cta_streams[agg_block_id];
    if(std::find(stream_list.begin(), stream_list.end(), stream) 
	    == stream_list.end())
	return false;
    else
	return true;
}

void kernel_info_t::print_parent_info() {
    if(m_parent_kernel) {
	printf("Parent %d: \'%s\', Agg Group %d, Block (%d, %d, %d), Thread (%d, %d, %d)\n", 
		m_parent_kernel->get_uid(), m_parent_kernel->name().c_str(), 
		m_parent_agg_group_id,
		m_parent_ctaid.x, m_parent_ctaid.y, m_parent_ctaid.z,
		m_parent_tid.x, m_parent_tid.y, m_parent_tid.z);
    }
}

void kernel_info_t::destroy_cta_streams() {
    printf("Destroy streams for kernel %d: ", get_uid());
    size_t stream_size = 0;
    for(auto s = m_cta_streams.begin(); s != m_cta_streams.end(); s++) {
	stream_size += s->second.size();
	if(s->second.size() != 0){
	for(auto ss = s->second.begin(); ss != s->second.end(); ss++){
//	    g_stream_manager->destroy_stream(*ss);
//            fprintf(stdout, "STREAMMANAGER, %d, %u, %u, %u, del, %0llx, %d\n", s->first.first, s->first.second.x, s->first.second.y, s->first.second.x, *ss, g_stream_manager->stream_count()); fflush(stdout);
        }
	}
	s->second.clear();
    }
    printf("size %lu\n", stream_size);
    m_cta_streams.clear();
}

//Jin: aggregated cta management
void kernel_info_t::add_agg_block_group(agg_block_group_t * agg_block_group) {

    dim3 block_dim = agg_block_group->get_block_dim();
    //for now, only support same cta configuration
    assert(block_dim.x == m_block_dim.x && block_dim.y == m_block_dim.y 
	    && block_dim.z == m_block_dim.z);

    m_total_num_agg_blocks += agg_block_group->get_num_blocks();

    assert(m_agg_block_groups.find(m_total_agg_group_id) ==
	    m_agg_block_groups.end());
    m_agg_block_groups[m_total_agg_group_id] = agg_block_group;

    printf("DTBL: kernel %u add agg group, totally %d\n", m_uid, m_total_agg_group_id);
    m_total_agg_group_id++;

    delete[] block_state;
    block_state = new block_info[num_blocks()];

    for(int i = 0; i < num_blocks(); i++){
       block_state[i].block_id.x = i;
       block_state[i].block_id.y = 0;
       block_state[i].block_id.z = 0;
       block_state[i].issued = 0;
       block_state[i].done = 0;
       block_state[i].switched = 0;
       block_state[i].preempted = 0;
       block_state[i].reissue = 0;
       block_state[i].time_stamp_switching = 0;
       block_state[i].time_stamp_switching_issue = 0;
       block_state[i].thread.set();
    }

    assert(unissued_agg_groups);
    unissued_agg_groups--;
    m_parent_threads.push_back(agg_block_group->get_parent_thd()); //record parent thread for parent-child dependency support
}

void kernel_info_t::destroy_agg_block_groups() {
    for(auto agg_block_group = m_agg_block_groups.begin(); 
	    agg_block_group != m_agg_block_groups.end();
	    agg_block_group++) {
	/* modeling the on-chip kernel queue */
#if 1
	extern bool *g_kernel_queue_entry_empty;
	extern unsigned int g_kernel_queue_entry_used;//, g_kernel_queue_entry_running; 
	if( agg_block_group->second->get_kernel_queue_entry_id() == -1 ){
	    extern unsigned long long total_num_offchip_metadata;
	    if(total_num_offchip_metadata > 0) total_num_offchip_metadata--;
	} else {
	    assert(g_kernel_queue_entry_empty[agg_block_group->second->get_kernel_queue_entry_id()] == false);
	    g_kernel_queue_entry_empty[agg_block_group->second->get_kernel_queue_entry_id()] = true;
	    g_kernel_queue_entry_used--;
//	    g_kernel_queue_entry_running--;
	    printf("DTBL: cycle %llu reclaim kernel queue entry %d\n", gpu_sim_cycle+gpu_tot_sim_cycle, agg_block_group->second->get_kernel_queue_entry_id());
	}
#endif
	extern int g_child_parameter_buffer_alignment;
	extern bool g_global_constant_pointer_sharing;
	extern int per_kernel_param_usage[12];
	extern application_id g_app_name;
	int param_buf_size;
	
	if(g_global_constant_pointer_sharing == 1)
	    param_buf_size = per_kernel_param_usage[g_app_name];
	else
	    param_buf_size = m_kernel_entry->get_args_aligned_size();

	g_total_param_size -= ((param_buf_size + g_child_parameter_buffer_alignment - 1)/ g_child_parameter_buffer_alignment * g_child_parameter_buffer_alignment);
	if(g_total_param_size < 0) g_total_param_size = 0;

	delete agg_block_group->second;
    }

    m_agg_block_groups.clear();
}

dim3 kernel_info_t::get_agg_dim(int agg_group_id) const {
    assert(m_agg_block_groups.find(agg_group_id) !=
	    m_agg_block_groups.end());
    return m_agg_block_groups.find(agg_group_id)->second->get_agg_dim();
}

class memory_space * kernel_info_t::get_agg_param_mem(int agg_group_id) {
    assert(m_agg_block_groups.find(agg_group_id) !=
	    m_agg_block_groups.end());
    return m_agg_block_groups.find(agg_group_id)->second->get_param_memory();
}

addr_t kernel_info_t::get_agg_param_mem_base(int agg_group_id) {
    assert(m_agg_block_groups.find(agg_group_id) !=
	    m_agg_block_groups.end());
    return m_agg_block_groups.find(agg_group_id)->second->get_param_memory_base();
}

void kernel_info_t::increment_cta_id() 
{ 
    dim3 next_grid_dim = get_grid_dim(m_next_agg_group_id);
    if(increment_x_then_y_then_z(m_next_cta, next_grid_dim)) { //overbound
	m_next_agg_group_id++;
	m_next_cta.x = 0;
	m_next_cta.y = 0;
	m_next_cta.z = 0;
	printf("DTBL: kernel %u overbound. next agg group %d total agg group %d\n", m_uid, m_next_agg_group_id, m_total_agg_group_id);

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
#if 0
Y		int kernel_queue_entry = m_agg_block_groups.find(m_next_agg_group_id)->second->get_kernel_queue_entry_id();
		printf("DTBL: cycle %llu next agg group %d queue entry %d\n", gpu_sim_cycle+gpu_tot_sim_cycle, m_next_agg_group_id, kernel_queue_entry);
		if( kernel_queue_entry == -1 ){ //next agg group info is stored in global memory
		    //				extern bool extra_metadata_load_latency;
		    extra_metadata_load_latency = true;
		    extern unsigned long long total_num_offchip_metadata;
		    if(total_num_offchip_metadata > 0) total_num_offchip_metadata--;
		} else {
		    extern bool *g_kernel_queue_entry_empty;
		    extern unsigned int g_kernel_queue_entry_used; 
		    assert(g_kernel_queue_entry_empty[kernel_queue_entry] == false);
		    g_kernel_queue_entry_empty[kernel_queue_entry] = true;
		    g_kernel_queue_entry_used--;
		    printf("DTBL: cycle %llu reclaim kernel queue entry %d\n", gpu_sim_cycle+gpu_tot_sim_cycle, kernel_queue_entry);
		}
#endif
	    }
	}
    }

    m_next_tid.x=0;
    m_next_tid.y=0;
    m_next_tid.z=0;
}

int kernel_info_t::get_kernel_queue_entry(int agg_group_id)
{
    return m_agg_block_groups.find(agg_group_id)->second->get_kernel_queue_entry_id();
}

simt_stack::simt_stack( unsigned wid, unsigned warpSize)
{
    m_warp_id=wid;
    m_warp_size = warpSize;
    reset();
}

void simt_stack::reset()
{
    m_stack.clear();
}

void simt_stack::launch( address_type start_pc, const simt_mask_t &active_mask )
{
    reset();
    simt_stack_entry new_stack_entry;
    new_stack_entry.m_pc = start_pc;
    new_stack_entry.m_calldepth = 1;
    new_stack_entry.m_active_mask = active_mask;
    new_stack_entry.m_type = STACK_ENTRY_TYPE_NORMAL;
    m_stack.push_back(new_stack_entry);
}

const simt_mask_t &simt_stack::get_active_mask() const
{
    assert(m_stack.size() > 0);
    return m_stack.back().m_active_mask;
}

void simt_stack::get_pdom_stack_top_info( unsigned *pc, unsigned *rpc ) const
{
    assert(m_stack.size() > 0);
    *pc = m_stack.back().m_pc;
    *rpc = m_stack.back().m_recvg_pc;
}

unsigned simt_stack::get_rp() const 
{ 
    assert(m_stack.size() > 0);
    return m_stack.back().m_recvg_pc;
}

void simt_stack::print (FILE *fout) const
{
    for ( unsigned k=0; k < m_stack.size(); k++ ) {
	simt_stack_entry stack_entry = m_stack[k];
	if ( k==0 ) {
	    fprintf(fout, "w%02d %1u ", m_warp_id, k );
	} else {
	    fprintf(fout, "    %1u ", k );
	}
	for (unsigned j=0; j<m_warp_size; j++)
	    fprintf(fout, "%c", (stack_entry.m_active_mask.test(j)?'1':'0') );
	fprintf(fout, " pc: 0x%03x", stack_entry.m_pc );
	if ( stack_entry.m_recvg_pc == (unsigned)-1 ) {
	    fprintf(fout," rp: ---- tp: %s cd: %2u ", (stack_entry.m_type==STACK_ENTRY_TYPE_CALL?"C":"N"), stack_entry.m_calldepth );
	} else {
	    fprintf(fout," rp: %4u tp: %s cd: %2u ", stack_entry.m_recvg_pc, (stack_entry.m_type==STACK_ENTRY_TYPE_CALL?"C":"N"), stack_entry.m_calldepth );
	}
	if ( stack_entry.m_branch_div_cycle != 0 ) {
	    fprintf(fout," bd@%6u ", (unsigned) stack_entry.m_branch_div_cycle );
	} else {
	    fprintf(fout," " );
	}
	ptx_print_insn( stack_entry.m_pc, fout );
	fprintf(fout,"\n");
    }
}

void simt_stack::update( simt_mask_t &thread_done, addr_vector_t &next_pc, address_type recvg_pc, op_type next_inst_op,unsigned next_inst_size, address_type next_inst_pc )
{
    assert(m_stack.size() > 0);

    assert( next_pc.size() == m_warp_size );

    simt_mask_t  top_active_mask = m_stack.back().m_active_mask;
    address_type top_recvg_pc = m_stack.back().m_recvg_pc;
    address_type top_pc = m_stack.back().m_pc; // the pc of the instruction just executed
    stack_entry_type top_type = m_stack.back().m_type;
    assert(top_pc==next_inst_pc);
    assert(top_active_mask.any());

    const address_type null_pc = -1;
    bool warp_diverged = false;
    address_type new_recvg_pc = null_pc;
    unsigned num_divergent_paths=0;

    std::map<address_type,simt_mask_t> divergent_paths;
    while (top_active_mask.any()) {

	// extract a group of threads with the same next PC among the active threads in the warp
	address_type tmp_next_pc = null_pc;
	simt_mask_t tmp_active_mask;
	for (int i = m_warp_size - 1; i >= 0; i--) {
	    if ( top_active_mask.test(i) ) { // is this thread active?
		if (thread_done.test(i)) {
		    top_active_mask.reset(i); // remove completed thread from active mask
		} else if (tmp_next_pc == null_pc) {
		    tmp_next_pc = next_pc[i];
		    tmp_active_mask.set(i);
		    top_active_mask.reset(i);
		} else if (tmp_next_pc == next_pc[i]) {
		    tmp_active_mask.set(i);
		    top_active_mask.reset(i);
		}
	    }
	}

	if(tmp_next_pc == null_pc) {
	    assert(!top_active_mask.any()); // all threads done
	    continue;
	}

	divergent_paths[tmp_next_pc]=tmp_active_mask;
	num_divergent_paths++;
    }


    address_type not_taken_pc = next_inst_pc+next_inst_size;
    assert(num_divergent_paths<=2);
    for(unsigned i=0; i<num_divergent_paths; i++){
	address_type tmp_next_pc = null_pc;
	simt_mask_t tmp_active_mask;
	tmp_active_mask.reset();
	if(divergent_paths.find(not_taken_pc)!=divergent_paths.end()){
	    assert(i==0);
	    tmp_next_pc=not_taken_pc;
	    tmp_active_mask=divergent_paths[tmp_next_pc];
	    divergent_paths.erase(tmp_next_pc);
	}else{
	    std::map<address_type,simt_mask_t>:: iterator it=divergent_paths.begin();
	    tmp_next_pc=it->first;
	    tmp_active_mask=divergent_paths[tmp_next_pc];
	    divergent_paths.erase(tmp_next_pc);
	}

	// HANDLE THE SPECIAL CASES FIRST
	if (next_inst_op== CALL_OPS){
	    // Since call is not a divergent instruction, all threads should have executed a call instruction
	    assert(num_divergent_paths == 1);

	    simt_stack_entry new_stack_entry;
	    new_stack_entry.m_pc = tmp_next_pc;
	    new_stack_entry.m_active_mask = tmp_active_mask;
	    new_stack_entry.m_branch_div_cycle = gpu_sim_cycle+gpu_tot_sim_cycle;
	    new_stack_entry.m_type = STACK_ENTRY_TYPE_CALL;
	    m_stack.push_back(new_stack_entry);
	    return;
	}else if(next_inst_op == RET_OPS && top_type==STACK_ENTRY_TYPE_CALL){
	    // pop the CALL Entry
	    assert(num_divergent_paths == 1);
	    m_stack.pop_back();

	    assert(m_stack.size() > 0);
	    m_stack.back().m_pc=tmp_next_pc;// set the PC of the stack top entry to return PC from  the call stack;
	    // Check if the New top of the stack is reconverging
	    if (tmp_next_pc == m_stack.back().m_recvg_pc && m_stack.back().m_type!=STACK_ENTRY_TYPE_CALL){
		assert(m_stack.back().m_type==STACK_ENTRY_TYPE_NORMAL);
		m_stack.pop_back();
	    }
	    return;
	}

	// discard the new entry if its PC matches with reconvergence PC
	// that automatically reconverges the entry
	// If the top stack entry is CALL, dont reconverge.
	if (tmp_next_pc == top_recvg_pc && (top_type != STACK_ENTRY_TYPE_CALL)) continue;

	// this new entry is not converging
	// if this entry does not include thread from the warp, divergence occurs
	if ((num_divergent_paths>1) && !warp_diverged ) {
	    warp_diverged = true;
	    // modify the existing top entry into a reconvergence entry in the pdom stack
	    new_recvg_pc = recvg_pc;
	    if (new_recvg_pc != top_recvg_pc) {
		m_stack.back().m_pc = new_recvg_pc;
		m_stack.back().m_branch_div_cycle = gpu_sim_cycle+gpu_tot_sim_cycle;

		m_stack.push_back(simt_stack_entry());
	    }
	}

	// discard the new entry if its PC matches with reconvergence PC
	if (warp_diverged && tmp_next_pc == new_recvg_pc) continue;

	// update the current top of pdom stack
	m_stack.back().m_pc = tmp_next_pc;
	m_stack.back().m_active_mask = tmp_active_mask;
	if (warp_diverged) {
	    m_stack.back().m_calldepth = 0;
	    m_stack.back().m_recvg_pc = new_recvg_pc;
	} else {
	    m_stack.back().m_recvg_pc = top_recvg_pc;
	}

	m_stack.push_back(simt_stack_entry());
    }
    assert(m_stack.size() > 0);
    m_stack.pop_back();


    if (warp_diverged) {
	ptx_file_line_stats_add_warp_divergence(top_pc, 1); 
    }
}

void core_t::execute_warp_inst_t(warp_inst_t &inst, unsigned warpId)
{
    for ( unsigned t=0; t < m_warp_size; t++ ) {
	if( inst.active(t) ) {
	    if(warpId==(unsigned (-1)))
		warpId = inst.warp_id();
	    unsigned tid=m_warp_size*warpId+t;
	    m_thread[tid]->ptx_exec_inst(inst,t);
	    //fprintf(stdout, "after ptx_exec_inst 2\n");

	    //virtual function
	    checkExecutionStatusAndUpdate(inst,t,tid);
	    //fprintf(stdout, "after checkUpdate\n");
	}
    } 
}

bool  core_t::ptx_thread_done( unsigned hw_thread_id ) const  
{
    return ((m_thread[ hw_thread_id ]==NULL) || m_thread[ hw_thread_id ]->is_done());
}

void core_t::updateSIMTStack(unsigned warpId, warp_inst_t * inst)
{
    simt_mask_t thread_done;
    addr_vector_t next_pc;
    unsigned wtid = warpId * m_warp_size;
    for (unsigned i = 0; i < m_warp_size; i++) {
	if( ptx_thread_done(wtid+i) ) {
	    thread_done.set(i);
	    next_pc.push_back( (address_type)-1 );
	} else {
	    if( inst->reconvergence_pc == RECONVERGE_RETURN_PC ) 
		inst->reconvergence_pc = get_return_pc(m_thread[wtid+i]);
	    next_pc.push_back( m_thread[wtid+i]->get_pc() );
	}
    }
    m_simt_stack[warpId]->update(thread_done,next_pc,inst->reconvergence_pc, inst->op,inst->isize,inst->pc);
}

//! Get the warp to be executed using the data taken form the SIMT stack
warp_inst_t core_t::getExecuteWarp(unsigned warpId)
{
    unsigned pc,rpc;
    m_simt_stack[warpId]->get_pdom_stack_top_info(&pc,&rpc);
    warp_inst_t wi= *ptx_fetch_inst(pc);
    wi.set_active(m_simt_stack[warpId]->get_active_mask());
    return wi;
}

void core_t::deleteSIMTStack()
{
    if ( m_simt_stack ) {
	for (unsigned i = 0; i < m_warp_count; ++i) 
	    delete m_simt_stack[i];
	delete[] m_simt_stack;
	m_simt_stack = NULL;
    }
}

void core_t::initilizeSIMTStack(unsigned warp_count, unsigned warp_size)
{ 
    m_simt_stack = new simt_stack*[warp_count];
    for (unsigned i = 0; i < warp_count; ++i) 
	m_simt_stack[i] = new simt_stack(i,warp_size);
    m_warp_size = warp_size;
    m_warp_count = warp_count;
}

void core_t::get_pdom_stack_top_info( unsigned warpId, unsigned *pc, unsigned *rpc ) const
{
    m_simt_stack[warpId]->get_pdom_stack_top_info(pc,rpc);
}
