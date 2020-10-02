#include "embedjoin.hpp"

using namespace cl::sycl;
using namespace oneapi::std;
using namespace std;

uint32_t samplingrange=0; // The maximum digit to embed, the range to sample
uint32_t countfilter=1;   // Number of required matches (>T) for a pair of substrings to be considered as candidate

size_t test_batches=2;

Time timer;


void setuplsh( int (*hash_lsh)[NUM_BITS], std::vector<int> &a, std::vector<int> &lshnumber, vector<tuple<int,int>> &rev_hash ){

	timer.start_time(init::init_lsh);

	for (int i = 0; i < NUM_HASH; i++){
		for (int j = 0; j < NUM_BITS; j++){
			hash_lsh[i][j] = rand() % (samplingrange);
		}
	}

	for (int i = 0; i < NUM_BITS; i++){
		a.push_back(rand() % (M - 1));
	}

	for (int i = 0; i < NUM_HASH; i++){
		for(int j=0; j < NUM_BITS; j++){
			lshnumber.emplace_back(hash_lsh[i][j]);
		}
	}

	tbb::parallel_sort(lshnumber.begin(), lshnumber.end());
	lshnumber.erase(unique(lshnumber.begin(), lshnumber.end()), lshnumber.end());
	samplingrange = lshnumber[lshnumber.size() - 1];

	for (int i = 0; i < NUM_HASH; i++){
		for (int j = 0; j < NUM_BITS; j++){
			hash_lsh[i][j] = lower_bound(lshnumber.begin(), lshnumber.end(), hash_lsh[i][j]) - lshnumber.begin();
		}
	}
	timer.end_time(init::init_lsh);

	/**
	 *  Compute the position in the embedded string for each lsh bit
	 * */
	timer.start_time(init::rev_lsh);

	rev_hash.resize(lshnumber.size(), make_tuple(-1,-1));
	int k=0;

	for(int i=0; i<NUM_HASH; i++){
		for(int j=0; j<NUM_BITS; j++){
			if(get<0>(rev_hash[hash_lsh[i][j]])!=-1){
				// Find last pos
				int t=hash_lsh[i][j];
				while(get<1>(rev_hash[t])!=-1){
					t=get<1>(rev_hash[t]);
				}
				rev_hash.emplace_back(make_tuple(k,-1));
				get<1>(rev_hash[t])=rev_hash.size()-1;
			}else {
				get<0>(rev_hash[hash_lsh[i][j]]) = k;
			}
			k++;
		}
	}
	timer.end_time(init::rev_lsh);
}


void read_dataset(vector<string>& input_data, string filename){

	std::cout<<"Reading dataset..."<<std::endl;
	ifstream data(filename);
	if(!data.is_open()){
		std::cerr<<"Error opening input file"<<std::endl;
		exit(-1);
	}

	string cell;
	int number_string = 0;
	while (getline(data, cell))
	{
		if(number_string==NUM_STRING){
			break;
		}
		number_string++;
		input_data.push_back(cell);
	}
}


void initialize_input_data(vector<string> &input_data, vector<size_t> &len_oristrings, char (*oristrings)[LEN_INPUT] ){

	auto start=std::chrono::system_clock::now();

	for(int i=0; i<NUM_STRING; i++){
		memset(oristrings[i],0,LEN_INPUT);
		strncpy(oristrings[i],input_data[i].c_str(),std::min(static_cast<int>(input_data[i].size()),LEN_INPUT));
		len_oristrings.emplace_back(input_data[i].size());
	}

	auto end=std::chrono::system_clock::now();

	std::cout<<"\nMemory op in read function: "<<(float)std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count()/1000<<std::endl;
	std::cout<<std::endl;
}


void initialization( vector<string> &input_data, std::vector<size_t> &len_oristrings, char (*oristrings)[LEN_INPUT], int (*hash_lsh)[NUM_BITS], std::vector<int> &a, std::vector<int> &lshnumber, vector<tuple<int,int>> &rev_hash ){

	timer.start_time(init::init_data);
	initialize_input_data(input_data, len_oristrings, oristrings);
	timer.end_time(init::init_data);
	setuplsh(hash_lsh, a, lshnumber, rev_hash);
}


void inititalize_dictory(uint8_t* dictory){

	if(NUM_CHAR==4){
		dictory[static_cast<uint8_t>('A')]=0;
		dictory[static_cast<uint8_t>('C')]=1;
		dictory[static_cast<uint8_t>('G')]=2;
		dictory[static_cast<uint8_t>('T')]=3;
	}else if(NUM_CHAR==5){
		dictory[static_cast<uint8_t>('A')]=0;
		dictory[static_cast<uint8_t>('C')]=1;
		dictory[static_cast<uint8_t>('G')]=2;
		dictory[static_cast<uint8_t>('T')]=3;
		dictory[static_cast<uint8_t>('N')]=4;
	}else if (NUM_CHAR == 26 || NUM_CHAR == 25){
		int j=0;
		for(int i=(int)'A'; i<=(int)'Z'; i++){
			dictory[i]=j;
			j++;
		}
	}
	else if (NUM_CHAR == 37){
		int j=0;
		for(int i=(int)'A'; i<=(int)'Z'; i++){
			dictory[i]=j;
			j++;
		}
		for(int i=(int)'0'; i<=(int)'9'; i++){
			dictory[i]=j;
			j++;
		}
		dictory[static_cast<uint8_t>(' ')]=j;
	}
	else{
		fprintf(stderr, "input error: check the dictory of your input\n");
	}
}


void allocate_work(vector<long> times, int num_dev, size_t units_to_allocate, vector<vector<size_t>> &size_per_dev){

	size_t idx_slowest=-1; // Id of slowest device
	size_t idx_fastest=-1; // Id of fastest device
	size_t n_fast=0;       // Number of batches to allocate to the fastest device
	size_t n_slow=0;       // Number of batches to allocate to the slowest device

	for(auto t:times){
		cout<<"\tTimes kernel: "<<(float)t/1000<<"sec"<<std::endl;
	}

	if(num_dev>1){
		// If there are 2 devices, compute the number of batches
		// to allocate to devices.
		// Note that at most 2 devices can be handled with this version of the function

		long slowest=-1;
		long fastest=-1;
		if(times[0]<=0 && times[1]<=0){
			slowest=1;
			fastest=1;
			idx_slowest=0;
			idx_fastest=1;
		}else if(times[0]<=0 && times[1]>0){
			slowest=1;
			idx_slowest=1;
			fastest=0;
			idx_fastest=0;
		}
		else if(times[0]>0 && times[1]<=0){
			slowest=1;
			idx_slowest=0;
			fastest=0;
			idx_fastest=1;
		}else{
			// Get the max and min time measured during profiling.
			// The max time is associated with the slowest device.
			// The min time is associated with the fastest device.
			// Get the position in the time vector corresponding
			// to the min and max time.
			// These positions correspond to the device positions
			// in the vector containing device queues.

			auto max_iter = std::max_element(times.begin(),times.end());
			slowest=*max_iter;
			idx_slowest=max_iter-times.begin();

			idx_fastest=1-idx_slowest;
			fastest=times[idx_fastest];
		}
		n_slow=floor(((float)fastest/(float)(fastest+slowest))*units_to_allocate);
		n_fast=units_to_allocate-n_slow;

		size_per_dev[idx_fastest].emplace_back(n_fast);
		size_per_dev[idx_slowest].emplace_back(n_slow);
	}else if(num_dev==1){
		// If there is only one device, all remaining batches
		// are given to the first (and only) device of the queue.

		idx_fastest=0;
		idx_slowest=-1;
		vector<size_t> tmp_sizes;
		n_slow=0;
		n_fast=units_to_allocate;
		size_per_dev[idx_fastest].emplace_back(n_fast);
	}
	cout<<"\n\tn_fast: "<<n_fast<<std::endl;
	cout<<"\tn_slow: "<<n_slow<<std::endl;

	cout<<"\tid_fastest: "<<idx_fastest<<std::endl;
	cout<<"\tid_slowest: "<<idx_slowest<<std::endl<<std::endl;

	int n=0;
	for(auto d:size_per_dev){
		cout<<"\tDev "<<n<<":"<<std::endl;
		int i=0;
		for(auto s:d){
			cout<<"\t\t"<<i<<". "<<s<<std::endl;
			i++;
		}
	}
}


void split_buffers(vector<vector<size_t>> &size_per_dev, size_t size_element, size_t limit=0xFFFFFFFF){

	int num_dev=size_per_dev.size();
	size_t tmp_size=0;

	if( num_dev>0 ){
		for(int d=0; d<num_dev; d++ ){
			if(size_per_dev[d].size()!=1){
				std::cout<<"ERROR: only one element should be in the vector at this point"<<std::endl;
				exit(-1);
			}
			size_t size=size_per_dev[d][0];
			size_t num_part=1;
			while( size*size_element/num_part > limit ){
				num_part++;
			}
			num_part++;
			std::cout<<"\n\tSplit buffer in "<<num_part<<" parts of "<<size/num_part<<" as dim."<<std::endl;
			size_per_dev[d].clear();
			for(int j=0; j<num_part; j++){
				if(j==num_part-1){
					size_per_dev[d].emplace_back(size/num_part+size%num_part);
				}
				else{
					size_per_dev[d].emplace_back(size/num_part);
				}
			}
		}
	}
}


void parallel_embedding(queue &device_queue, buffer<size_t,1> &buffer_len_oristrings, buffer<char,2> &buffer_oristrings, buffer<char,1> &buffer_embdata, size_t batch_size, buffer<int,1> &buffer_lshnumber, buffer<int,1> &buffer_p, buffer<size_t,1> &buffer_len_output, buffer<uint32_t,1> &buffer_samplingrange, buffer<uint8_t,1> &buffer_dict, buffer<std::tuple<int,int>> &buffer_rev_hash){

	std::cout << "\n\t\tTask: Embedding Data";
	std::cout << "\tDevice: " << device_queue.get_device().get_info<info::device::name>() << std::endl;

	device_queue.submit([&](handler &cgh){

		auto acc_oristrings = buffer_oristrings.get_access<access::mode::read>(cgh);
		auto acc_lshnumber = buffer_lshnumber.get_access<access::mode::read,access::target::constant_buffer>(cgh);
		auto acc_embdata = buffer_embdata.get_access<access::mode::write>(cgh);
		auto acc_dict = buffer_dict.get_access<access::mode::read>(cgh);
		auto acc_samplingrange = buffer_samplingrange.get_access<access::mode::read>(cgh);
		auto acc_len_oristrings = buffer_len_oristrings.get_access<access::mode::read>(cgh);
		auto acc_p=buffer_p.get_access<access::mode::read>(cgh);
		auto acc_len_output=buffer_len_output.get_access<access::mode::read>(cgh);
		auto acc_rev_hash=buffer_rev_hash.get_access<access::mode::read>(cgh);

		std::cout<<"\t\t\tBatch size: "<<batch_size<<std::endl;
		std::cout<<"\t\t\tRange: ("<<batch_size<<", "<<NUM_STR<<", "<<NUM_REP<<")"<<std::endl;

		//Executing kernel
		cgh.parallel_for<class EmbedString>(range<3>{batch_size,NUM_STR,NUM_REP}, [=](id<3> index){

			int id = index[0];
			int l = index[1];
			int k = index[2];

			int partdigit = 0;
			int size = acc_len_oristrings[id];
			int r = 0;
			int len_out = acc_lshnumber.get_range()[0];
			int i = SHIFT*k;
			int len=acc_samplingrange[0]+1;

			for(int j = 0; i < size && j <= acc_samplingrange[0]; i++){

				char s = acc_oristrings[id][i];
				r = acc_dict[s];

				j += ( acc_p[ABSPOS_P(l,r,j,len)] + 1 );

				while (partdigit < len_out && j > acc_lshnumber[partdigit]){

					acc_embdata[ABSPOS(id,l,k,std::get<0>(acc_rev_hash[partdigit]),acc_len_output[0])]=s;
					int next=std::get<1>(acc_rev_hash[partdigit]);

					while(next!=-1){
						acc_embdata[ABSPOS(id,l,k,std::get<0>(acc_rev_hash[next]),acc_len_output[0])]=s;
						next=get<1>(acc_rev_hash[next]);
					}
				  partdigit++;
				}
			}
		});
	});
}


void  create_buckets(queue &device_queue, char **embdata, buffer<buckets_t,1> &buffer_buckets, buffer<size_t,1> &buffer_batch_size, size_t split_size, buffer<size_t,1> &buffer_split_offset, buffer<uint32_t,1> &buffer_a, buffer<size_t,1> &buffer_len_output, buffer<uint8_t,1> &buffer_dict){

	std::cout << "\n\tTask: Buckets Generation\t";
	std::cout << "Device: " << device_queue.get_device().get_info<info::device::name>() << std::endl;
	std::cout<<"\t\tSplit size: "<<split_size<<std::endl;

	range<2> glob_range(split_size*NUM_STR*NUM_REP,NUM_HASH);
	range<3> local_range(250,1,1);

	std::cout<<"\t\tGlobal range: "<<"("<<glob_range[0]<<", "<<glob_range[1]<<")"<<std::endl;
	{
		device_queue.submit([&](handler &cgh){

			auto acc_buckets = buffer_buckets.get_access<access::mode::write>(cgh);
			auto acc_dict = buffer_dict.get_access<access::mode::read>(cgh);
			auto acc_a = buffer_a.get_access<access::mode::read>(cgh);
			auto acc_batch_size=buffer_batch_size.get_access<access::mode::read>(cgh);
			auto acc_len_output=buffer_len_output.get_access<access::mode::read>(cgh);
			auto acc_split_offset=buffer_split_offset.get_access<access::mode::read>(cgh);

			//Executing kernel
			cgh.parallel_for<class CreateBuckets>(range<2>(glob_range), [=](item<2> index){

				int itq=index[0];
				int i=itq/(NUM_STR*NUM_REP)+acc_split_offset[0];
				int tq=itq%(NUM_STR*NUM_REP);
				int t=tq/NUM_REP;
				int q=tq%NUM_REP;

				int k=index[1];

				int id=0;
				char dict_index=0;
				int id_mod=0;
				int digit=-1;

				for (int j = 0; j < NUM_BITS; j++){
					digit=k*NUM_BITS+j;
					dict_index=embdata[(int)(i/acc_batch_size[0])][ABSPOS((int)(i%acc_batch_size[0]),t,q,digit,acc_len_output[0])];
					id += (acc_dict[dict_index]) * acc_a[j];
				}

				id_mod=id % M;
				size_t output_position=index.get_linear_id();

				get<0>(acc_buckets[output_position])=t;
				get<1>(acc_buckets[output_position])=k;
				get<2>(acc_buckets[output_position])=id_mod;
				get<3>(acc_buckets[output_position])=i;
				get<4>(acc_buckets[output_position])=q;
			});
		});
	}
}


void create_buckets_wrapper(vector<queue> &queues, char **embdata, vector<buckets_t> &buckets, size_t n_batches, size_t batch_size, int* hash_lsh, vector<int> &a, vector<int> &lshnumber, size_t len_output){

	std::cout<< "\nCreate buckets"<<std::endl;
	std::cout<<"\n\tLen output: "<<len_output<<std::endl;

	int num_dev=queues.size();

	/*
	 * Number batches to use for profiling:
	 * 2 batches per queue/device
	 */
	int number_of_testing_batches=2*num_dev;
	vector<long> times;
	{
		vector<size_t> split_size;
		uint8_t dictory[256]={0};

		inititalize_dictory(dictory);

		std::vector<vector<size_t>> size_per_dev(num_dev, vector<size_t>(test_batches,1));
		vector<size_t> offset(1,0);
		vector<sycl::buffer<buckets_t>> buffers_buckets;
		vector<sycl::buffer<size_t,1>> buffers_batch_size;
		vector<sycl::buffer<size_t,1>> buffers_split_size;
		vector<sycl::buffer<size_t,1>> buffers_split_offset;
		vector<sycl::buffer<uint32_t,2>> buffers_hash_lsh;
		vector<sycl::buffer<uint32_t,1>> buffers_a;
		vector<sycl::buffer<uint32_t,1>> buffers_lshnumber;
		vector<sycl::buffer<size_t,1>> buffers_len_output;
		vector<sycl::buffer<uint8_t,1>>  buffers_dict;

		timer.start_time(buckets::measure);

		int n=0; // Global number of iteration
		int dev=0; // Device index

		cout<<"\n\tStart profiling on devices..."<<std::endl;
		/**
		 * Profiling kernel on devices by using the test batches;
		 * Allocate work based on performances
		 * Run kernel for remaining data
		 * */
		bool is_profiling=true;
		while(dev<queues.size()){
			int iter=0;
			while(iter<size_per_dev[dev].size() && size_per_dev[dev][iter]>0){

				// Two kernel are chosen, since the first one
				// includes kernel compiling time

				auto start=std::chrono::system_clock::now();

				split_size.emplace_back(size_per_dev[dev][iter]*batch_size);
				offset.emplace_back(offset.back()+(n==0?0:split_size[n-1]));
				size_t loc_split_size=split_size[n];

				cout<<"\n\tSet offset to: "<<offset.back()<<std::endl;

				buffers_buckets.emplace_back(sycl::buffer<buckets_t,1>(static_cast<buckets_t*>(buckets.data()+offset.back()*NUM_REP*NUM_HASH*NUM_STR),range<1>{loc_split_size*NUM_STR*NUM_HASH*NUM_REP}, {sycl::property::buffer::use_host_ptr()}));
				buffers_a.emplace_back(buffer<uint32_t,1>((uint32_t*)a.data(),range<1>{a.size()}));
				buffers_hash_lsh.emplace_back(buffer<uint32_t,2>((uint32_t*)hash_lsh, range<2>{NUM_HASH,NUM_BITS}));
				buffers_dict.emplace_back(buffer<uint8_t,1>(dictory,range<1>{256}));
				buffers_batch_size.emplace_back(buffer<size_t,1>(&batch_size, range<1>{1}));
				buffers_len_output.emplace_back(buffer<size_t,1>(&len_output, range<1>{1}));
				buffers_split_offset.emplace_back(buffer<size_t,1> (&offset.back(), range<1>{1}));

				create_buckets(queues[dev], embdata, buffers_buckets[n], buffers_batch_size[n], loc_split_size, buffers_split_offset[n], buffers_a[n], buffers_len_output[n], buffers_dict[n]);

				if(is_profiling){
					queues[dev].wait();
				}
				auto end=std::chrono::system_clock::now();

				// Save the time only for the second kernel execution for each device
				// because the first run includes the compiling time
				if(iter==test_batches-1 && is_profiling){
					times.emplace_back(std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count());
				}
				n++;
				iter++;
			}
			dev++;
			if(dev==queues.size() && is_profiling){
				timer.end_time(buckets::measure); // End profiling
				is_profiling=false;
				dev=0;
				for(int l=0; l<num_dev; l++){
					size_per_dev[l].clear();
				}
				allocate_work(times,num_dev,n_batches-number_of_testing_batches, size_per_dev);
				timer.start_time(buckets::compute); // Start actual computing
			}
		}
	}
	timer.end_time(buckets::compute);
}


void generate_candidates(queue &device_queue, buffer<size_t,1> &buffer_len_oristrings, char **embdata, buffer<buckets_t,1> &buffer_buckets, buffer<size_t,1> &buffer_buckets_offset, buffer<size_t,1> &buffer_batch_size, buffer<candidate_t,1> &buffer_candidates, size_t candidate_size, buffer<size_t,1> &buffer_len_output ){

	cout << "\n\t\tTask: Candidate Pairs Generation\t";
	std::cout << "Device: " << device_queue.get_device().get_info<info::device::name>() << std::endl;

	device_queue.submit([&](handler &cgh){

		auto acc_buckets = buffer_buckets.get_access<access::mode::read>(cgh);
		auto acc_candidate = buffer_candidates.get_access<access::mode::write>(cgh);
		auto acc_len = buffer_len_oristrings.get_access<access::mode::read>(cgh);
		auto acc_batch_size=buffer_batch_size.get_access<access::mode::read>(cgh);
		auto acc_len_output=buffer_len_output.get_access<access::mode::read>(cgh);
		auto acc_buckets_offset=buffer_buckets_offset.get_access<access::mode::read>(cgh);

		std::cout<<"\t\t\tCandidate size: "<<candidate_size<<std::endl;

		cgh.parallel_for<class GenerateCandidates>(range<1>(candidate_size), [=](item<1> index){

			int ij = index[0];
			int index_output=ij;

			int sum=0;

			size_t tmp_i=get<0>(acc_candidate[ij]);
			size_t tmp_j=get<1>(acc_candidate[ij]);
			size_t buck_off=acc_buckets_offset[0];

			size_t i=get<0>(acc_candidate[ij])-acc_buckets_offset[0];//begin+i_norm;
			size_t j=get<1>(acc_candidate[ij])-acc_buckets_offset[0];//begin+i_norm;

			int t1=get<0>(acc_buckets[i]);
			int k1=get<1>(acc_buckets[i]);

			int i1=get<3>(acc_buckets[i]);
			int q1=get<4>(acc_buckets[i]);

			int i2=get<3>(acc_buckets[j]);
			int q2=get<4>(acc_buckets[j]);

			__int8_t c1;
			__int8_t c2;

			for (int j = k1*NUM_BITS; j < k1*NUM_BITS+NUM_BITS; j++){

				c1=embdata[i1/acc_batch_size[0]][ ABSPOS(i1%acc_batch_size[0],t1,q1,j,acc_len_output[0]) ];
				c2=embdata[i2/acc_batch_size[0]][ ABSPOS(i2%acc_batch_size[0],t1,q2,j,acc_len_output[0]) ];

				if(c1!=0 && c2!=0){
					sum+=abs_diff(c1,c2);
				}
			}

			/***
			 * q12 is made (b7)( b6, b5, b4 )( b3, b2, b1)(b0)
			 * 			(unused) (q1) (q2) (compare result)
			 */
			uint8_t q12=(uint8_t)q1;
			q12=q12<<3;
			q12=q12+q2;
			q12=q12<<1;
			q12=q12+(sum>0?1:0);

			get<0>(acc_candidate[index_output])=i1;
			get<1>(acc_candidate[index_output])=abs_diff(acc_len[i1], acc_len[i2]);
			get<2>(acc_candidate[index_output])=i2;
			get<3>(acc_candidate[index_output])=q12;
		});
	});
}


void generate_candidates_wrapper(vector<queue>& queues, vector<size_t> &len_oristrings, char* oristrings, char **embdata, vector<buckets_t> &buckets, size_t batch_size, vector<candidate_t>& candidate, int * local_hash_lsh, vector<int> &lshnumber, size_t len_output){

	cout <<"\nGenerate candidates"<< std::endl;
	cout<<"\n\tLen output: "<<len_output<<std::endl;

	{
		int num_dev=queues.size();

		// Select a number of candidates to use for profiling.
		// The size can change:
		// too big can let to a big overhead
		// too small can reduce the quality of profiling
		size_t size_for_test=0.01*candidate.size();
		vector<vector<size_t>> size_cand(num_dev,vector<size_t>(test_batches,size_for_test));
		vector<uint32_t> number_of_iter(num_dev);
		vector<size_t> buckets_offset(1000);

		vector<buffer<buckets_t>> buffers_buckets;
		vector<buffer<int, 2>> buffers_hash_lsh;
		vector<buffer<candidate_t>> buffers_candidates;
		vector<buffer<size_t,1>> buffers_len;
		vector<buffer<size_t, 1>> buffers_batch_size;
		vector<buffer<size_t, 1>> buffers_len_output;
		vector<buffer<size_t,1>> buffers_buckets_offset;

		vector<long> times;
		cout<<"\nSize (num candidates) for profiling: "<<size_for_test<<std::endl;

		timer.start_time(cand::measure);
		cout<<"\n\tStart profiling..."<<std::endl;

		/**
		 *
		 * Profiling kernel on devices by using the test batches;
		 * Allocate work based on performances
		 * Run kernel for remaining data
		 *
		 * */
		size_t offset_cand=0;
		int dev=0;
		int n=0;
		bool is_profiling=true;
		while(dev<queues.size()){
			int iter=0;
			while(iter<size_cand[dev].size() && size_cand[dev][iter]>0){
				auto start=std::chrono::system_clock::now();

				cout<<"\n\tSize cand[dev]: "<<size_cand[dev][iter]<<std::endl;

				size_t start_b=get<0>(candidate[offset_cand]);
				size_t end_b=get<2>((candidate.data()+offset_cand)[size_cand[dev][iter]-1])-1;
				size_t size_buckets=end_b-start_b+1;

				buckets_offset[n]=start_b;

				cout<<"\n\tIter "<<dev<<". Start buckets at "<<offset_cand<<": "<<start_b<<std::endl;
				cout<<"\tIter "<<dev<<". End buckets at "<<offset_cand + size_cand[dev][iter]-1<<": "<<end_b<<std::endl;
				cout<<"\n\tBuckets size: "<<size_buckets<<std::endl;
				cout<<"\n\tBuckets offset: "<<buckets_offset.back()<<std::endl;
				cout<<"\tCand size: "<<size_cand[dev][iter]<<std::endl;
				cout<<"\tOffset: "<<offset_cand<<std::endl;

				buffers_buckets.emplace_back( buffer<buckets_t>(buckets.data()+start_b,range<1>{size_buckets}));
				buffers_hash_lsh.emplace_back( buffer<int, 2>(reinterpret_cast<int*>(local_hash_lsh),range<2>{NUM_HASH,NUM_BITS}));
				buffers_candidates.emplace_back( buffer<candidate_t>(candidate.data()+offset_cand,range<1>{size_cand[dev][iter]}));
				buffers_len.emplace_back( buffer<size_t,1>(len_oristrings.data(),range<1>{len_oristrings.size()}));
				buffers_batch_size.emplace_back( buffer<size_t, 1>(&batch_size,range<1>{1}));
				buffers_len_output.emplace_back( buffer<size_t, 1>(&len_output,range<1>{1}));
				buffers_buckets_offset.emplace_back( buffer<size_t,1>(&buckets_offset[n],range<1>{1}));

				generate_candidates(queues[dev], buffers_len[n], embdata, buffers_buckets[n], buffers_buckets_offset[n], buffers_batch_size[n], buffers_candidates[n], size_cand[dev][iter], buffers_len_output[n]);

				if(is_profiling){
					queues[dev].wait();
				}
				auto end=std::chrono::system_clock::now();

				if(iter==test_batches-1 && is_profiling){
					times.emplace_back(std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count());
				}
				offset_cand+=size_cand[dev][iter];
				n++;
				iter++;
			}
			dev++;
			if(dev==queues.size() && is_profiling){
				timer.end_time(cand::measure); // End profiling
				is_profiling=false;
				dev=0;
				for(int l=0; l<num_dev; l++){
					size_cand[l].clear();
				}
				size_t remaining_size=candidate.size()-size_for_test*test_batches*num_dev;
				allocate_work(times,num_dev,remaining_size, size_cand);
				cout<<"\tRemaining size: "<<remaining_size<<std::endl;
				split_buffers(size_cand, sizeof(candidate[0]));
				timer.start_time(cand::compute); // Start actual computing
			}
		}
	}
	timer.end_time(cand::compute);
}


void generate_random_string(int* p, int len_p){

	for (int j = 0; j < NUM_STR; j++) {
		for (int t = 0; t < NUM_CHAR; t++){
			for (int d = 0; d < samplingrange + 1; d++){
				p[ABSPOS_P(j,t,d,len_p)]=1-rand() % 2;
			}
			for (int d = 0; d < samplingrange + 1; d++){
				if(p[ABSPOS_P(j,t,d,len_p)]==1){
					if(d>0 && p[ABSPOS_P(j,t,d-1,len_p)]==1){
						p[ABSPOS_P(j,t,d,len_p)] = p[ABSPOS_P(j,t,d-1,len_p)] - 1;
					}
					else{
						int next = d+1;
						while(next < samplingrange + 1 && p[ABSPOS_P(j,t,next,len_p)]==1){
							p[ABSPOS_P(j,t,d,len_p)]++;
							next++;
						}
					}
				}
			}
		}
	}
}


void initialize_candidate_pairs(vector<queue>& queues, vector<buckets_t> &buckets, vector<candidate_t> &candidates ){

	cout<<"\nInitialize candidate vector"<<std::endl;

	/*
	 * Compute the boundary ( starting index and size ) of each buckets in the 1-D vector
	 * */
	timer.start_time(cand_init::comp_buck_delim);

	vector<tuple<int,int>> buckets_delimiter;
	int j=0;
	buckets_delimiter.emplace_back(make_tuple(0,0));
	for(int i=0; i<buckets.size()-1; i++){
		get<1>(buckets_delimiter[j])++;
		if( (get<0>(buckets[i])!=get<0>(buckets[i+1]))
						|| (get<0>(buckets[i])==get<0>(buckets[i+1]) && get<1>(buckets[i])!=get<1>(buckets[i+1]) )
						|| (get<0>(buckets[i])==get<0>(buckets[i+1]) && get<1>(buckets[i])==get<1>(buckets[i+1]) && get<2>(buckets[i])!=get<2>(buckets[i+1])) ){
			j++;
			buckets_delimiter.emplace_back(make_tuple(i+1,0));
		}
	}
	timer.end_time(cand_init::comp_buck_delim);
	std::cout<<"\n\tTime cand-init: count element: "<<(float)timer.get_step_time(cand_init::comp_buck_delim)<<"sec"<<std::endl;

	timer.start_time(cand_init::filter_buck_delim);
	/**
	 * Remove buckets having size == 1, since no candidates are possible
	 * */
	std::cout<<"\t\tSize before remove: "<<buckets_delimiter.size()<<std::endl;
	auto new_end=remove_if(oneapi::dpl::execution::par, buckets_delimiter.begin(),buckets_delimiter.end(),[](std::tuple<int,int> e){return std::get<1>(e)<2;});
	buckets_delimiter.erase( new_end, buckets_delimiter.end());
	std::cout<<"\t\tSize after remove: "<<buckets_delimiter.size()<<std::endl;

	timer.end_time(cand_init::filter_buck_delim);
	std::cout<<"\n\tTime cand-init: remove element: "<<(float)timer.get_step_time(cand_init::filter_buck_delim)<<"sec"<<std::endl;

	/**
	 * Since each buckets has a variable number of possible candidates,
	 * count the maximum number of candidates by scanning the array.
	 * This allows us to set approximately the same number of candidates for each kernel.
	 * This is not possible if the split was based ony on buckets delimiters
	 * */
	size_t size=0;
	for(size_t b=0; b<buckets_delimiter.size(); b++){
		size_t n=get<1>(buckets_delimiter[b]);
		size+=((n*(n-1))/2);
	}
	std::cout<<"\n\tSize to allocate: "<<size<<std::endl;
	try{
		timer.start_time(cand_init::resize);
		candidates.resize(size);
		timer.end_time(cand_init::resize);
		std::cout<<"\tTime cand-init: resize vector: "<<(float)timer.get_step_time(cand_init::resize)<<"sec"<<std::endl;

		timer.start_time(cand_init::scan_cand);

		size_t c=0;
		for(auto &b:buckets_delimiter ){
			size_t start=get<0>(b);
			size_t size=get<1>(b);
			size_t end=start+size;

			for(size_t i=start; i<end-1; i++){
				for(size_t j=i+1; j<end; j++ ){
					get<0>(candidates[c])=i;
					get<1>(candidates[c])=j;
					get<2>(candidates[c])=end;
					c++;
				}
			}
		}
		timer.end_time(cand_init::scan_cand);
		if(c!=size){
			cout<<c<<" != "<<size<<std::endl;
			cout<<"Exit"<<std::endl;
			exit(-1);
		}
		cout<<c<<" == "<<size<<std::endl;
	}
	catch(std::exception& e){
		std::cout<<"Too many candidates. Reduce the number of input strings"<<std::endl;
		std::cout<<"or find the parameter to spread better strings accros hash buckets"<<std::endl;
		exit(-1);
	}
	std::cout<<"\tTime cand-init: assign i and j to candidates: "<<(float)timer.get_step_time(cand_init::scan_cand)<<"sec"<<std::endl;
}


void parallel_embedding_wrapper(std::vector<queue> &queues, vector<size_t> &len_oristrings, char (*oristrings)[LEN_INPUT], char** set_embdata_dev, size_t batch_size, size_t n_batches, std::vector<int> &lshnumber, size_t &len_output, std::vector<tuple<int,int>> &rev_hash){

	std::cout<< "Parallel Embedding"<<std::endl;
	/**
	 * Initialize the "dictory" that contains the translation
	 * character -> number
	 * */
	uint8_t dictory[256]={0};
	inititalize_dictory(dictory);

	cout<<"\n\tLen output: "<<len_output<<std::endl;
	timer.start_time(embed::rand_str);
	/**
	 * Allocate and initialize random strings to use for embedding
	 * **/
	uint32_t len_p=samplingrange+1;
	int *p=new int[NUM_STR*NUM_CHAR*len_p];
	generate_random_string(p, len_p);
	timer.end_time(embed::rand_str);

	int num_dev=queues.size();

	// Number batches to use for profiling
	// (2 batches per queue/device)
	int number_of_testing_batches=2*num_dev;

	// Store the time taken by each device to execute 1 kernel
	std::vector<long> times;
	{
		/**
		 * Each queue and each kernel has its own copy of data (sycl::buffer).
		 * Also read-only data shared by all kernels, are replicated once for
		 * each kernel, in order to reduce dependencies among different kernels
		 * and between device queues.
		 * Thus, a vector of sycl::buffer is created for each array accessed
		 * in the kernel.
		 *
		 * Buffer are created inside the scope of this function, so buffer destructor
		 * is used as synch method to put back data in the host
		 * */
		std::vector<buffer<int,1>> buffers_p;
		std::vector<buffer<char,2>> buffers_oristrings;
		std::vector<buffer<int,1>> buffers_lshnumber;
		std::vector<buffer<char,1>> buffers_embdata;
		std::vector<buffer<uint8_t,1>> buffers_dict;
		std::vector<buffer<size_t,1>> buffers_len_oristrings;
		std::vector<buffer<uint32_t,1>> buffers_samplingrange;
		std::vector<buffer<size_t,1>> buffers_len_output;
		std::vector<buffer<tuple<int,int>>> buffers_rev_hash;

		int n=0;   // number of iterations
		int dev=0; // device index
		//For each device, there are at least some test iteration.
		std::vector<vector<size_t>> size_per_dev(num_dev, vector<size_t>(1,test_batches));

		std::cout<<"\tStart profiling on devices..."<<std::endl;
		/**
		 * Profiling kernel on devices by using the test batches.
		 * The test is executed on the 2 devices sequentially, waiting
		 * at the end of each testing kernel.
		 * Allocate work based on performances
		 * Run kernels for remaining data
		 * */
		timer.start_time(embed::measure);
		bool is_profiling=true;
		int state=0;
		while(dev<queues.size()){

			// Two kernel are chosen, since the first one
			// includes kernel compiling time
			int iter=0;
			while(iter<size_per_dev[dev].back()){

				auto start=std::chrono::system_clock::now();

				size_t size_p=static_cast<size_t>(NUM_STR*NUM_CHAR*(samplingrange+1));
				size_t size_emb=static_cast<size_t>(batch_size*NUM_STR*NUM_REP*len_output);
				uint32_t samprange=samplingrange;

				buffers_p.emplace_back( buffer<int,1>(p, range<1>{size_p}) );
				buffers_oristrings.emplace_back( buffer<char, 2>(reinterpret_cast<char*>((char*)oristrings[n*batch_size]),range<2>{batch_size,LEN_INPUT}) );
				buffers_lshnumber.emplace_back( buffer<int, 1>(lshnumber.data(),range<1>{lshnumber.size()}) );
				buffers_embdata.emplace_back( buffer<char, 1> (reinterpret_cast<char*>(set_embdata_dev[n]), range<1>{size_emb}, {property::buffer::use_host_ptr()}) );
				buffers_dict.emplace_back( buffer<uint8_t,1>(dictory,range<1>{256}) );
				buffers_len_oristrings.emplace_back( buffer<size_t,1>(len_oristrings.data()+n*batch_size,range<1>(batch_size)) );
				buffers_samplingrange.emplace_back( buffer<uint32_t,1>(&samprange,range<1>(1)) );
				buffers_len_output.emplace_back( buffer<size_t, 1>(&len_output,range<1>{1}) );
				buffers_rev_hash.emplace_back( buffer<tuple<int,int>>(rev_hash.data(),range<1>(rev_hash.size())));

				parallel_embedding( queues[dev], buffers_len_oristrings[n], buffers_oristrings[n], buffers_embdata[n], batch_size, buffers_lshnumber[n], buffers_p[n], buffers_len_output[n], buffers_samplingrange[n], buffers_dict[n], buffers_rev_hash[n]);

				if(is_profiling){
					queues[dev].wait();
				}
				auto end=std::chrono::system_clock::now();

				if(iter==test_batches-1 && is_profiling){
					times.emplace_back(std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count());
				}
				n++;
				iter++;
			}
			dev++;
			if(dev==queues.size() && is_profiling){
				timer.end_time(embed::measure); // End profiling
				is_profiling=false;
				dev=0;
				allocate_work(times,num_dev,n_batches-number_of_testing_batches, size_per_dev);
				timer.start_time(embed::compute); // Start actual computing
			}
		}
	} // End of scope: sync with host here to measure computing time
	timer.end_time(embed::compute);
	delete[] p;
}


void print_output( vector<string> &input_data, vector<idpair> &output_pairs, string out_filename ){

	std::cout<<"Start saving results"<<std::endl;
	ofstream out_file;
	out_file.open(out_filename, ios::out | ios::trunc);

	if (!out_file.is_open()) {
		std::cerr<<"Not possible to open file"<<std::endl;
		exit(-1);
	}

	tbb::parallel_sort(output_pairs.begin(), output_pairs.end());
	output_pairs.erase(unique(output_pairs.begin(), output_pairs.end()), output_pairs.end());

	if(ALLOUTPUTRESULT) {
		for (int i = 0; i < output_pairs.size(); i++) {
			out_file << get<0>(output_pairs[i]) << " " << get<1>(output_pairs[i]) << std::endl;
			out_file << input_data[get<0>(output_pairs[i])] << std::endl;
			out_file << input_data[get<1>(output_pairs[i])] << std::endl;
		}
	}
}


std::string getReportFileName(vector<queue>&queues, int device, size_t batch_size){

	std::string report_name="";
	if(device==0){
		report_name+="-CPU-";
	}else if(device==1){
		report_name+="-GPU-";
	}else if(device==2){
		report_name+="-BOTH-";
	}
	else{
		report_name+="-ERROR";
	}
	report_name+=std::to_string(batch_size);
	return report_name;
}


void onejoin(vector<string> &input_data, size_t batch_size, size_t n_batches, int device, uint32_t new_samplingrange, uint32_t new_countfilter, Time &t, string dataset_name) {

	samplingrange=new_samplingrange;
	countfilter=new_countfilter;
	size_t len_output=NUM_HASH*NUM_BITS;
	timer=t;

	print_configuration(batch_size, n_batches, len_output, countfilter, samplingrange);

	auto asyncHandler = [&](cl::sycl::exception_list eL) {
		for (auto& e : eL) {
			try {
				std::rethrow_exception(e);
			}catch (cl::sycl::exception& e) {
				std::cout << e.what() << std::endl;
				std::cout << "fail" << std::endl;
				std::terminate();
			}
		}
	};

	// VARIABLES:
	/* HASH
	 * a: the random vector for second level hash table
	 * lshnumber: all distinct lsh bits actually used
	 * hash_lsh: matrix of hash functions and their bits
	 * rev_hash: vector containing the position of lsh bits into an embedded string
	 */
	vector<int> a;
	vector<int> lshnumber;
	int (*hash_lsh)[NUM_BITS];
	vector<tuple<int,int>> rev_hash;

	/**
	 * INPUT:
	 * oristrings: array of rows, containing the input strings to use inside the embed kernel
	 * len_oristrings: actual len of each input string
	 * */

	char (*oristrings)[LEN_INPUT];
	vector<size_t> len_oristrings;

	/**
	 * OUTPUT:
	 * output_pairs: contains the IDs of strings in the input vector
	 * 				 that are similar according edit distance
	 * */
	vector<idpair> output_pairs;

	/*
	 * OTHER:
	 * buckets: vector containing lsh buckets;
	 * 			the first 3 elements identify the buckets;
	 * 			the 4th and 5th element are respectively
	 * 			the input string id and the replication id.
	 *
	 * candidates:  vector containing the candidates pairs of strings
	 * 				that will be processed and verified by means of
	 * 				edit distance computaton;
	 * 				the 1st and 3rd element contain the id of input strings
	 * 				of a candidate pair;
	 * 				the 2nd element contains the difference of lenghts of 2 strings
	 * 				the 4th element in an uint32_t  type and contains use 3 bits to
	 * 				contain the replication id of first string, 3 bit for the
	 * 				replication id of second string, and 1 bits that say if the 2 strings
	 * 				have all lsh bits equal.
	 *
	 * queues: vector containing the sycl device queues.
	 * */
	vector<buckets_t> buckets;
	vector<candidate_t> candidates;
	vector<queue> queues;
	try{
		hash_lsh = new int[NUM_HASH][NUM_BITS];
		oristrings = new char[NUM_STRING][LEN_INPUT];
		buckets.resize(NUM_STRING*NUM_STR*NUM_HASH*NUM_REP);

	}catch(std::bad_alloc& e){
		std::cerr<<"It is not possible allocate the requested size."<<std::endl;
		exit(-1);
	}
	if(device==0 || device==2){ // Selected CPU or both
		queues.push_back(queue(cpu_selector{}, asyncHandler, property::queue::in_order()));
	}
	if(device==1 || device==2){ // Selected GPU or both
		try{
			queue tmp_queue(gpu_selector{}, asyncHandler, property::queue::in_order());
			queues.push_back(std::move(tmp_queue));

		}catch(std::exception& e){ // No GPU available, use CPU if not selected yet
			std::cout<<"Attention: no GPU device detected. The program will run on CPU."<<std::endl;

			if( queues.size()==0 ){
				queues.push_back(queue(cpu_selector{}, asyncHandler, property::queue::in_order()));
			}
		}
	}
	cout<<"\nNumber of devices: "<<queues.size()<<std::endl<<std::endl;

	/**
	 * INITIALIZATION
	 * */
	timer.start_time(total_alg::total);
	timer.start_time(init::total);

	srand(11110);
	initialization(input_data, len_oristrings, oristrings, hash_lsh, a, lshnumber, rev_hash);

	timer.end_time(init::total);
	std::cerr << "Start parallel algorithm..." << std::endl<<std::endl;

	/**
	 * EMBEDDING STEP
	 **/
	timer.start_time(embed::total);

	timer.start_time(embed::alloc);
	char **set_embdata_dev=(char**)malloc_shared<char*>(n_batches, queues.back());
	for(int n=0; n<n_batches; n++){
		set_embdata_dev[n]=malloc_shared<char>(batch_size*NUM_STR*NUM_REP*len_output, queues.back());
		memset(set_embdata_dev[n],0,batch_size*NUM_STR*NUM_REP*len_output);
	}
	timer.end_time(embed::alloc);

	parallel_embedding_wrapper(queues, len_oristrings, oristrings, set_embdata_dev, batch_size, n_batches, lshnumber, len_output, rev_hash);

	for(auto &q : queues){
		q.wait();
	}

	timer.end_time(embed::total);

	delete[] oristrings;
	cout<<"Time: "<<timer.get_step_time(embed::total)<<"sec"<<std::endl;

#if PRINT_EMB
		print_embedded( set_embdata_dev, len_output, batch_size, string("embedded"+to_string(device)+".txt"));
#endif

	timer.start_time(total_join::total);
	/**
	 * CREATE BUCKETS STEP
	 * **/

	timer.start_time(buckets::total);

	create_buckets_wrapper(queues, (char**)set_embdata_dev, buckets, n_batches, batch_size, (int*)hash_lsh, a, lshnumber, len_output);

	for(auto &q:queues){
		q.wait();
	}

	timer.end_time(buckets::total);
	cout<<"Time buckets creation: "<<timer.get_step_time(buckets::total)<<"sec"<<std::endl;

	timer.start_time(sort_buckets::total);

	tbb::parallel_sort(buckets.begin(), buckets.end());

	timer.end_time(sort_buckets::total);
	std::cout<<"\nSorting buckets: "<<timer.get_step_time(sort_buckets::total)<<std::endl;

#if PRINT_BUCK
	 print_buckets(buckets, string("buckets"+to_string(device)+".txt"));
#endif

	 /**
	  * INITIALIZATION FOR CANDIDATE GENERATION
	  * **/

	 timer.start_time(cand_init::total);

	 initialize_candidate_pairs( queues, buckets, candidates );

	 timer.end_time(cand_init::total);

	 std::cout<<timer.get_step_time(cand_init::total)<<std::endl;

	 /**
	 * GENERATE CANDIDATE PAIRS STEP
	 * **/

	 timer.start_time(cand::total);

	 generate_candidates_wrapper(queues, len_oristrings, (char*)oristrings, (char**)set_embdata_dev, buckets, batch_size, candidates, (int *)hash_lsh, lshnumber, len_output);

	 timer.end_time(cand::total);

	 for(auto &q:queues){
		 q.wait();
	 }

	 /**
	  * Since buckets and embed strings are not used anymore,
	  * their memory is released before continuing
	  * */
	buckets.clear();
	for(int i=0; i<n_batches; i++){
		if(set_embdata_dev[i]==nullptr){
			cout<<"ERROR: Null pointer!"<<std::endl;
		}else{
			free(set_embdata_dev[i], queues.back());
		}
	}
	if(set_embdata_dev==nullptr){
				cout<<"ERROR: Null pointer!"<<std::endl;
	}else{
		free(set_embdata_dev, queues.back());
	}
	cout<<"Clear buckets"<<std::endl;
	cout<<"Delete embdata"<<std::endl;

	/**
	  * CANDIDATES PROCESSING
	  * */
	timer.start_time(cand_proc::total);
	std::cout<<"\n\nStarting candidate processing analysis..."<<std::endl;
	std::cout<<"\n\t\tCandidates size: "<<candidates.size()<<std::endl;

	timer.start_time(cand_proc::rem_cand);
	vector<std::tuple<int,int>> verifycan;
	try{
		candidates.erase(std::remove_if(oneapi::dpl::execution::par_unseq, candidates.begin(), candidates.end(),[](candidate_t e){return (get<1>(e)>K_INPUT || (get<3>(e) & 0x1)!=0 || get<0>(e)==get<2>(e));}), candidates.end());
	}catch(std::exception &e){
		std::cout<<"Error in remove function. Too many candidates for the parallel version."<<std::endl;
		std::cout<<"The sequential version will be used."<<std::endl;
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(),[](candidate_t e){return (get<1>(e)>K_INPUT || (get<3>(e) & 0x1)!=0 || get<0>(e)==get<2>(e));}), candidates.end());
	}
	timer.end_time(cand_proc::rem_cand);

	timer.start_time(cand_proc::sort_cand);
	tbb::parallel_sort( candidates.begin(), candidates.end() );
	timer.end_time(cand_proc::sort_cand);

#if PRINT_CAND
	print_candidate_pairs(candidates, string("candidates"+to_string(device)));
#endif

	/*
	 * COUNTING FREQUENCIES
	 * **/

	timer.start_time(cand_proc::count_freq);
	std::vector<int> freq_uv;
	if (!candidates.empty())
	{
		freq_uv.push_back(0);
		auto prev = candidates[0];
		for (auto const & x : candidates)
		{
			if (prev != x)
			{
				freq_uv.push_back(0);
				prev = x;
			}
			++freq_uv.back();
		}
	}
	timer.end_time(cand_proc::count_freq);

	timer.start_time(cand_proc::rem_dup);
	candidates.erase(unique( candidates.begin(), candidates.end() ), candidates.end());
	timer.end_time(cand_proc::rem_dup);

	timer.start_time(cand_proc::filter_low_freq);
	for (int i = 0; i < candidates.size(); i++){
		if (freq_uv[i] > countfilter ){
			verifycan.emplace_back(get<0>(candidates[i]),get<2>(candidates[i]));
		}
	}
	timer.end_time(cand_proc::filter_low_freq);

	timer.start_time(cand_proc::sort_cand_to_verify);
	tbb::parallel_sort(verifycan.begin(), verifycan.end());
	timer.end_time(cand_proc::sort_cand_to_verify);

	timer.start_time(cand_proc::make_uniq);
	verifycan.erase(unique(verifycan.begin(), verifycan.end()), verifycan.end());
	timer.end_time(cand_proc::make_uniq);

	cout<<"\nEnd candidates processing"<<std::endl;
	timer.end_time(cand_proc::total);

	/**
	 * EDIT DISTANCE
	 * */
	timer.start_time(edit_dist::total);

	uint32_t num_threads = std::thread::hardware_concurrency();
	cout<<"\nNumber of threads for edit distance: "<<num_threads<<std::endl;

	std::vector<std::thread> workers;
	std::atomic<size_t> verified(0);
	size_t to_verify=verifycan.size();
	uint32_t num_outputs=0;
	uint32_t num_candidates=0;
	std::mutex mt;
	std::cout<<"\n\tTo verify: "<<to_verify<<std::endl;

	for(int t=0; t<num_threads; t++){
		workers.push_back(std::thread([&](){

			while(true){
				int j=verified.fetch_add(1);
				if(j<to_verify){

					int first_str;
					int second_str;

					first_str=get<0>(verifycan[j]);
					second_str=get<1>(verifycan[j]);

					string tmp_str1=input_data[first_str];
					string tmp_str2=input_data[second_str];

					for(int k = 0;k < 8; k++){
						tmp_str1.push_back(j>>(8*k));
						tmp_str2.push_back(j>>(8*k));
					}
					int ed = edit_distance(tmp_str2.data(), len_oristrings[second_str], tmp_str1.data(), len_oristrings[first_str] /*tmp_oridata[first_str].size()*/, K_INPUT);

					std::unique_lock<std::mutex> lk(mt);
					if(ed != -1) {
						num_outputs++;
						output_pairs.push_back(make_tuple(first_str, second_str));
					}
					num_candidates++;
				}
				else{
					break;
				}
			}
		}));
	}

	for(auto &t:workers){
		if(t.joinable()){
			t.join();
		}
	}

	timer.end_time(edit_dist::total);
	cout<<"\n\t\tNum output: "<<num_outputs<<std::endl;

	timer.end_time(total_join::total);
	timer.end_time(total_alg::total);

	delete[] hash_lsh;
	cout<<"\nDelete hash_lsh"<<std::endl;

	timer.print_summary(num_candidates,num_outputs);

	string report_name=getReportFileName(queues, device, batch_size);
	{
		ofstream out_file;
		out_file.open("report-"+dataset_name+report_name, ios::out | ios::trunc);
		std::string dev="";

		if(device==2){
			int count_dev=0;
			for(auto &q : queues){
				dev+=q.get_device().get_info<info::device::name>();
				dev+=count_dev==(queues.size()-1)?"": " && ";
				count_dev++;
			}
		}else{
			dev=queues.back().get_device().get_info<info::device::name>();
		}

		if (out_file.is_open()) {
			timer.print_report(dev, num_candidates, num_outputs, out_file);
		}
	}
	print_output(input_data, output_pairs, "join_output_parallel.txt");
}
