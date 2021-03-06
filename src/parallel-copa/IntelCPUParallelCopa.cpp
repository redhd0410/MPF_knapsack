#include "IntelCPUParallelCopa.hpp"
#include <algorithm>
#include <vector>
#include <bitset>
#include <CL/cl.hpp>

#include <common/Timer.hpp>

#include "Pair.hpp"



cl_long IntelCPUParallelCopa::Solve()
{
	timer.start();
	// Split A and B, sort
	split_vector();

	// setup opencl
	const auto src = GetSourceFromFile(cl_file_path);
	auto sources = cl::Program::Sources(1, std::make_pair(src.c_str(), src.length() + 1));
	program = cl::Program(context, sources);

	if (program.build({ device }, "-D __linux__") != CL_SUCCESS)
	{
		std::cout << "Error log:\n" << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device) << std::endl;
		exit(EXIT_FAILURE);
	}

	// init constants
	ASize = A.size();
	BSize = B.size();
	A_buffer_size = (1ll << ASize);
	B_buffer_size = (1ll << BSize);
	A_local_size = A_buffer_size / num_parallel;
	B_local_size = B_buffer_size / num_parallel;

	// init buffers
	cl_int err;
	buffer_A = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(Triple) * A_buffer_size, NULL, &err);
	printError(err, "buffer_A");
	buffer_B = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(Triple) * B_buffer_size, NULL, &err);
	printError(err, "buffer_B");



	// stage 1: parallel generation
	parallel_generation();
	// stage 2: first max scan
	first_max_scan();
	// stage 3: prune
	prune();
	// stage 4: second max scan
	second_max_scan();
	// stage 5: final_search
	final_search();

	elapsedTime = timer.stop();

	return solution_value;
}



void IntelCPUParallelCopa::split_vector() noexcept
{
	const size_t ASize = (data.getTable().size() >> 1);
	const size_t BSize = data.getTable().size() - ASize;
	A.reserve(ASize);
	B.reserve(BSize);
	for (size_t i = 0; i < ASize; ++i)
	{
		const auto& entry = data.getTable().at(i);
		A.emplace_back(1ll << i, entry.first, entry.second);
	}
	for (size_t i = ASize; i < data.getTable().size(); ++i)
	{
		const auto& entry = data.getTable().at(i);
		B.emplace_back(1ll << i, entry.first, entry.second);
	}
	std::sort(A.begin(), A.end(), [](const Triple& a, const Triple& b) { return a.w < b.w; });
	std::sort(B.begin(), B.end(), [](const Triple& a, const Triple& b) { return a.w > b.w; });
}

void IntelCPUParallelCopa::parallel_generation()
{
	cl_int err;
	// buffer init
	cl::Buffer buffer_tmp = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(Triple) * (1ll << ((ASize > BSize ? ASize : BSize) - 1)), NULL, &err);
	printError(err, "buffer_tmp");
	cl::Buffer buffer_tmp2 = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(Triple) * (1ll << ((ASize > BSize ? ASize : BSize) - 1)), NULL, &err);
	printError(err, "buffer_tmp2");
	cl::Buffer buffer_arg = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(Triple), NULL, &err);
	printError(err, "buffer_arg");
	// initial values
	const Triple init{ 1, 0, 0 };
	err = queue.enqueueWriteBuffer(buffer_A, CL_FALSE, 0, sizeof(Triple), &init);
	printError(err, "enqueue initial value to buffer_A");
	err = queue.enqueueWriteBuffer(buffer_B, CL_FALSE, 0, sizeof(Triple), &init);
	printError(err, "enqueue initial value to buffer_B");
	// define common kernels
	cl::Kernel add_triple(program, "add_triple", &err);
	printError(err, "add_triple kernel error");
	add_triple.setArg(0, buffer_tmp);
	add_triple.setArg(1, buffer_tmp2);
	add_triple.setArg(2, buffer_arg);
	// set sequential threshold
	int seq_threshold = 0;
	cl_long _num_parallel = num_parallel;
	while (_num_parallel) {
		_num_parallel >>= 1;
		++seq_threshold;
	}

	// ----------------------------
	// Parallel Generation for A
	// ----------------------------
	cl::Kernel merge_array_sequential_a(program, "merge_array_sequential_a", &err);
	printError(err, "merge_array_sequential_a kernel error");
	merge_array_sequential_a.setArg(0, buffer_A);
	merge_array_sequential_a.setArg(1, buffer_tmp);
	merge_array_sequential_a.setArg(2, buffer_tmp2);
	cl::Kernel merge_array_parallel_a(program, "merge_array_parallel_a");
	merge_array_parallel_a.setArg(0, buffer_A);
	merge_array_parallel_a.setArg(1, buffer_tmp);
	merge_array_parallel_a.setArg(2, buffer_tmp2);
	for (size_t i = 0, seq_size = (ASize < seq_threshold ? ASize : seq_threshold); i < seq_size; ++i)
	{
		// sequnetial merge
		const cl_long size = (1ll << i);
		const cl_long bytesize = sizeof(Triple) * size;
		queue.enqueueCopyBuffer(buffer_A, buffer_tmp, 0, 0, bytesize);
		queue.enqueueWriteBuffer(buffer_arg, CL_FALSE, 0, sizeof(Triple), &(A.at(i)));
		queue.enqueueNDRangeKernel(add_triple,
			cl::NullRange,
			cl::NDRange(size),
			cl::NDRange(size));
		merge_array_sequential_a.setArg(3, sizeof(cl_long), &size);
		queue.enqueueNDRangeKernel(merge_array_sequential_a,
			cl::NullRange,
			cl::NDRange(1),
			cl::NDRange(1));
		
		// debug only -----------------------------------------------------------
		//printBuffer(size, bytesize);
		// debug only -----------------------------------------------------------
	}
	queue.finish(); // synchronization point
	for (int i = seq_threshold; i < ASize; ++i)
	{
		const cl_long size = (1ll << i);
		const cl_long bytesize = sizeof(Triple) * size;
		queue.enqueueCopyBuffer(buffer_A, buffer_tmp, 0, 0, bytesize);
		queue.enqueueWriteBuffer(buffer_arg, CL_FALSE, 0, sizeof(Triple), &(A.at(i)));
		add_triple.setArg(2, buffer_arg);
		queue.enqueueNDRangeKernel(add_triple,
			cl::NullRange,
			cl::NDRange(size),
			cl::NDRange(size / num_parallel));
		queue.finish();
		for (_num_parallel = num_parallel; _num_parallel > 0; _num_parallel >>= 1) {
			cl_long local_size = size / _num_parallel;
			merge_array_parallel_a.setArg(3, sizeof(cl_long), &local_size);
			queue.enqueueNDRangeKernel(merge_array_parallel_a,
				cl::NullRange,
				cl::NDRange(_num_parallel),
				cl::NDRange(1));
			queue.finish();
		}
		// debug only -----------------------------------------------------------
		//printBuffer(size, bytesize);
		// debug only -----------------------------------------------------------
	}
	// ----------------------------
	// Parallel Generation for B
	// ----------------------------
	cl::Kernel merge_array_sequential_b(program, "merge_array_sequential_b", &err);
	printError(err, "merge_array_sequential_b kernel error");
	merge_array_sequential_b.setArg(0, buffer_B);
	merge_array_sequential_b.setArg(1, buffer_tmp);
	merge_array_sequential_b.setArg(2, buffer_tmp2);
	cl::Kernel merge_array_parallel_b(program, "merge_array_parallel_b");
	merge_array_parallel_b.setArg(0, buffer_B);
	merge_array_parallel_b.setArg(1, buffer_tmp);
	merge_array_parallel_b.setArg(2, buffer_tmp2);
	for (size_t i = 0, seq_size = (BSize < seq_threshold ? BSize : seq_threshold); i < seq_size; ++i)
	{
		// sequnetial merge
		const cl_long size = (1ll << i);
		const cl_long bytesize = sizeof(Triple) * size;
		queue.enqueueCopyBuffer(buffer_B, buffer_tmp, 0, 0, bytesize);
		queue.enqueueWriteBuffer(buffer_arg, CL_FALSE, 0, sizeof(Triple), &(B.at(i)));
		queue.enqueueNDRangeKernel(add_triple,
			cl::NullRange,
			cl::NDRange(size),
			cl::NDRange(size));
		merge_array_sequential_b.setArg(3, sizeof(cl_long), &size);
		queue.enqueueNDRangeKernel(merge_array_sequential_b,
			cl::NullRange,
			cl::NDRange(1),
			cl::NDRange(1));
		queue.finish(); // synchronization point
		// debug only -----------------------------------------------------------
		//printBuffer(size, bytesize);
		// debug only -----------------------------------------------------------
	}
	for (int i = seq_threshold; i < BSize; ++i)
	{
		const cl_long size = (1ll << i);
		const cl_long bytesize = sizeof(Triple) * size;
		queue.enqueueCopyBuffer(buffer_B, buffer_tmp, 0, 0, bytesize);
		queue.enqueueWriteBuffer(buffer_arg, CL_FALSE, 0, sizeof(Triple), &(B.at(i)));
		add_triple.setArg(2, buffer_arg);
		queue.enqueueNDRangeKernel(add_triple,
			cl::NullRange,
			cl::NDRange(size),
			cl::NDRange(size / num_parallel));
		queue.finish();
		for (_num_parallel = num_parallel; _num_parallel > 0; _num_parallel >>= 1) {
			cl_long local_size = size / _num_parallel;
			merge_array_parallel_b.setArg(3, sizeof(cl_long), &local_size);
			queue.enqueueNDRangeKernel(merge_array_parallel_b,
				cl::NullRange,
				cl::NDRange(_num_parallel),
				cl::NDRange(1));
			queue.finish();
		}
		// devel only -----------------------------------------------------------
		//printBuffer(size, bytesize);
		// devel only -----------------------------------------------------------
	}
	//printBuffer(A)
}


void IntelCPUParallelCopa::first_max_scan()
{
	cl_int err;
	const size_t bytesize = sizeof(cl_long) * num_parallel;

	const cl_long ABufferSize = (1ll << A.size()) / num_parallel;
	buffer_AMaxValues = cl::Buffer(context, CL_MEM_READ_WRITE, bytesize);
	buffer_AMaxValuesIdx = cl::Buffer(context, CL_MEM_READ_WRITE, bytesize);
	cl::Kernel get_max_value_a(program, "first_max_scan", &err);
	printError(err, "get_max_value_a kernel error");
	get_max_value_a.setArg(0, buffer_A);
	get_max_value_a.setArg(1, buffer_AMaxValues);
	get_max_value_a.setArg(2, buffer_AMaxValuesIdx);
	get_max_value_a.setArg(3, sizeof(cl_long), &ABufferSize);
	queue.enqueueNDRangeKernel(get_max_value_a,
		cl::NullRange,
		cl::NDRange(num_parallel),
		cl::NDRange(1));

	const cl_long BBufferSize = (1ll << B.size()) / num_parallel;
	buffer_BMaxValues = cl::Buffer(context, CL_MEM_READ_WRITE, bytesize);
	buffer_BMaxValuesIdx = cl::Buffer(context, CL_MEM_READ_WRITE, bytesize);
	cl::Kernel get_max_value_b(program, "first_max_scan", &err);
	printError(err, "get_max_value_b kernel error");
	get_max_value_b.setArg(0, buffer_B);
	get_max_value_b.setArg(1, buffer_BMaxValues);
	get_max_value_b.setArg(2, buffer_BMaxValuesIdx);
	get_max_value_b.setArg(3, sizeof(cl_long), &BBufferSize);
	queue.enqueueNDRangeKernel(get_max_value_b,
		cl::NullRange,
		cl::NDRange(num_parallel),
		cl::NDRange(1));
	queue.finish();
	 /*printBuffer<cl_long>(num_parallel, buffer_AMaxValuesIdx, "buffer_AMaxValuesIdx");
	 printBuffer<cl_long>(num_parallel, buffer_BMaxValuesIdx, "buffer_BMaxValuesIdx");*/
}


void IntelCPUParallelCopa::prune()
{
	const size_t buffers_pruned_bytesize = sizeof(Pair) * num_parallel * 2;
	buffer_pruned = cl::Buffer(context, CL_MEM_READ_WRITE, buffers_pruned_bytesize);
	queue.enqueueFillBuffer(buffer_pruned, -1, 0, buffers_pruned_bytesize);
	buffer_first_max_val = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(cl_long)*num_parallel);
	buffer_first_max_val_pair = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(cl_long) * num_parallel);
	cl_int err;
	cl::Kernel prune_kernel(program, "prune", &err);
	printError(err, "error in prune_parallel ");

	const cl_long c = data.getCapacity();
	prune_kernel.setArg(0, buffer_A);
	prune_kernel.setArg(1, buffer_B);
	prune_kernel.setArg(2, buffer_pruned);
	prune_kernel.setArg(3, buffer_AMaxValues);
	prune_kernel.setArg(4, buffer_BMaxValues);
	prune_kernel.setArg(5, buffer_AMaxValuesIdx);
	prune_kernel.setArg(6, buffer_BMaxValuesIdx);
	prune_kernel.setArg(7, buffer_first_max_val);
	prune_kernel.setArg(8, buffer_first_max_val_pair);
	prune_kernel.setArg(9, sizeof(cl_long), &A_local_size);
	prune_kernel.setArg(10, sizeof(cl_long), &B_local_size);
	prune_kernel.setArg(11, sizeof(cl_long), &c);
	prune_kernel.setArg(12, sizeof(cl_long), &num_parallel);
	queue.enqueueNDRangeKernel(prune_kernel,
		cl::NullRange,
		cl::NDRange(num_parallel),
		cl::NDRange(1));
	queue.finish();
	// printBuffer<Pair>(num_parallel * 2, buffer_pruned,"after prunning");
}

void IntelCPUParallelCopa::second_max_scan()
{
	const size_t buffer_max_size = sizeof(cl_long) * 2 * B_local_size * num_parallel;
	cl_int err;
	buffer_max = cl::Buffer(context, CL_MEM_READ_WRITE, buffer_max_size, NULL, &err);
	queue.enqueueFillBuffer(buffer_max, -1, 0, buffer_max_size);
	printError(err, "buffer_max");
	cl::Kernel second_max_scan_kernel(program, "second_max_scan", &err);
	printError(err, "error in second_max_scan");
	second_max_scan_kernel.setArg(0, buffer_A);
	second_max_scan_kernel.setArg(1, buffer_B);
	second_max_scan_kernel.setArg(2, buffer_pruned);
	second_max_scan_kernel.setArg(3, buffer_max);
	second_max_scan_kernel.setArg(4, sizeof(cl_long), &A_local_size);
	second_max_scan_kernel.setArg(5, sizeof(cl_long), &B_local_size);
	//printBuffer<Pair>(2 * num_parallel, buffer_pruned, "second_max_scan");
	queue.enqueueNDRangeKernel(second_max_scan_kernel,
		cl::NullRange,
		cl::NDRange(num_parallel),
		cl::NDRange(1));
	queue.finish();
	
}

void IntelCPUParallelCopa::final_search()
{
	const size_t buffer_max_bytesize = num_parallel * sizeof(cl_long);
	const size_t buffer_max_pair_bytesize = num_parallel * sizeof(Pair);
	cl_int err;
	buffer_second_max_val = cl::Buffer(context, CL_MEM_READ_WRITE, buffer_max_bytesize, NULL, &err);
	printError(err, "buffer_max_val");
	buffer_second_max_val_pair = cl::Buffer(context, CL_MEM_READ_WRITE, buffer_max_pair_bytesize, NULL, &err);
	printError(err, "buffer_max_val_pair");
	const cl_long c = data.getCapacity();
	cl::Kernel final_search_kernel(program, "final_search", &err);
	printError(err, "error in final_search");
	final_search_kernel.setArg(0, buffer_A);
	final_search_kernel.setArg(1, buffer_B);
	final_search_kernel.setArg(2, buffer_pruned);
	final_search_kernel.setArg(3, buffer_max);
	final_search_kernel.setArg(4, buffer_second_max_val);
	final_search_kernel.setArg(5, buffer_second_max_val_pair);
	final_search_kernel.setArg(6, sizeof(cl_long), &A_local_size);
	final_search_kernel.setArg(7, sizeof(cl_long), &B_local_size);
	final_search_kernel.setArg(8, sizeof(cl_long), &c);
	final_search_kernel.setArg(9, sizeof(cl_long), &num_parallel);
	queue.enqueueNDRangeKernel(final_search_kernel,
		cl::NullRange,
		cl::NDRange(num_parallel),
		cl::NDRange(1));

	// find solution in first scan
	std::vector<cl_long> max_val(num_parallel);
	std::vector<cl_long> max_val_set(num_parallel);
	queue.enqueueReadBuffer(buffer_first_max_val, CL_FALSE, 0, buffer_max_bytesize, max_val.data());
	queue.enqueueReadBuffer(buffer_first_max_val_pair, CL_TRUE, 0, buffer_max_bytesize, max_val_set.data());
	int maxi = 0; solution_value = max_val.at(0);
	for (int i = 1; i < max_val.size(); ++i)
	{
		if (max_val.at(i) > solution_value) {
			solution_value = max_val.at(i);
			maxi = i;
		}
	}
	solution_set = std::bitset<64>(max_val_set.at(maxi));

	// find solution in second scan
	std::vector<Pair> max_val_pair(num_parallel);
	queue.enqueueReadBuffer(buffer_second_max_val, CL_FALSE, 0, buffer_max_bytesize, max_val.data());
	queue.enqueueReadBuffer(buffer_second_max_val_pair, CL_TRUE, 0, buffer_max_pair_bytesize, max_val_pair.data());
	maxi = -1;
	for (int i = 0; i < max_val.size(); ++i)
	{
		if (max_val.at(i) >= solution_value)
		{
			solution_value = max_val.at(i);
			maxi = i;
		}
	}
	if (maxi >= 0) {
		solution_set = std::bitset<64>(max_val_pair.at(maxi).a_idx + max_val_pair.at(maxi).b_idx);
	}
}