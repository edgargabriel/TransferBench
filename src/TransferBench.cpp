/*
Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

// This program measures simultaneous copy performance across multiple GPUs
// on the same node
#include <numa.h>     // If not found, try installing libnuma-dev (e.g apt-get install libnuma-dev)
#include <cmath>      // If not found, try installing g++-12      (e.g apt-get install g++-12)
#include <numaif.h>
#include <random>
#include <stack>
#include <thread>

#include "TransferBench.hpp"
#include "GetClosestNumaNode.hpp"

int main(int argc, char **argv)
{
  // Check for NUMA library support
  if (numa_available() == -1)
  {
    printf("[ERROR] NUMA library not supported. Check to see if libnuma has been installed on this system\n");
    exit(1);
  }

  // Display usage instructions and detected topology
  if (argc <= 1)
  {
    int const outputToCsv = EnvVars::GetEnvVar("OUTPUT_TO_CSV", 0);
    if (!outputToCsv) DisplayUsage(argv[0]);
    DisplayTopology(outputToCsv);
    exit(0);
  }

  // Collect environment variables / display current run configuration
  EnvVars ev;

  // Determine number of bytes to run per Transfer
  size_t numBytesPerTransfer = argc > 2 ? atoll(argv[2]) : DEFAULT_BYTES_PER_TRANSFER;
  if (argc > 2)
  {
    // Adjust bytes if unit specified
    char units = argv[2][strlen(argv[2])-1];
    switch (units)
    {
    case 'K': case 'k': numBytesPerTransfer *= 1024; break;
    case 'M': case 'm': numBytesPerTransfer *= 1024*1024; break;
    case 'G': case 'g': numBytesPerTransfer *= 1024*1024*1024; break;
    }
  }
  if (numBytesPerTransfer % 4)
  {
    printf("[ERROR] numBytesPerTransfer (%lu) must be a multiple of 4\n", numBytesPerTransfer);
    exit(1);
  }

  // Check for preset tests
  // - Tests that sweep across possible sets of Transfers
  if (!strcmp(argv[1], "sweep") || !strcmp(argv[1], "rsweep"))
  {
    int numGpuSubExecs = (argc > 3 ? atoi(argv[3]) : 4);
    int numCpuSubExecs = (argc > 4 ? atoi(argv[4]) : 4);

    ev.configMode = CFG_SWEEP;
    RunSweepPreset(ev, numBytesPerTransfer, numGpuSubExecs, numCpuSubExecs, !strcmp(argv[1], "rsweep"));
    exit(0);
  }
  // - Tests that benchmark peer-to-peer performance
  else if (!strcmp(argv[1], "p2p"))
  {
    ev.configMode = CFG_P2P;
    RunPeerToPeerBenchmarks(ev, numBytesPerTransfer / sizeof(float));
    exit(0);
  }
  // - Test SubExecutor scaling
  else if (!strcmp(argv[1], "scaling"))
  {
    int maxSubExecs = (argc > 3 ? atoi(argv[3]) : 32);
    int exeIndex    = (argc > 4 ? atoi(argv[4]) : 0);

    if (exeIndex >= ev.numGpuDevices)
    {
      printf("[ERROR] Cannot execute scaling test with GPU device %d\n", exeIndex);
      exit(1);
    }
    ev.configMode = CFG_SCALE;
    RunScalingBenchmark(ev, numBytesPerTransfer / sizeof(float), exeIndex, maxSubExecs);
    exit(0);
  }
  // - Test all2all benchmark
  else if (!strcmp(argv[1], "a2a"))
  {
    int numSubExecs = (argc > 3 ? atoi(argv[3]) : 4);

    // Force single-stream mode for all-to-all benchmark
    ev.useSingleStream = 1;
    ev.configMode = CFG_A2A;
    RunAllToAllBenchmark(ev, numBytesPerTransfer, numSubExecs);
    exit(0);
  }
  else if (!strcmp(argv[1], "cmdline"))
  {
    // Print environment variables and CSV header
    ev.DisplayEnvVars();
    if (ev.outputToCsv)
    {
      printf("Test#,Transfer#,NumBytes,Src,Exe,Dst,CUs,BW(GB/s),Time(ms),SrcAddr,DstAddr\n");
    }

    // Read Transfer from command line
    std::string cmdlineTransfer;
    for (int i = 3; i < argc; i++)
      cmdlineTransfer += std::string(argv[i]) + " ";

    char line[MAX_LINE_LEN];
    sprintf(line, "%s", cmdlineTransfer.c_str());
    std::vector<Transfer> transfers;
    ParseTransfers(line, ev.numCpuDevices, ev.numGpuDevices, transfers);
    if (transfers.empty()) exit(0);

    // If the number of bytes is specified, use it
    if (numBytesPerTransfer != 0)
    {
      size_t N = numBytesPerTransfer / sizeof(float);
      ExecuteTransfers(ev, 1, N, transfers);
    }
    else
    {
      // Otherwise generate a range of values
      for (int N = 256; N <= (1<<27); N *= 2)
      {
        int delta = std::max(1, N / ev.samplingFactor);
        int curr = N;
        while (curr < N * 2)
        {
          ExecuteTransfers(ev, 1, curr, transfers);
          curr += delta;
        }
      }
    }
    exit(0);
  }

  // Check that Transfer configuration file can be opened
  ev.configMode = CFG_FILE;
  FILE* fp = fopen(argv[1], "r");
  if (!fp)
  {
    printf("[ERROR] Unable to open transfer configuration file: [%s]\n", argv[1]);
    exit(1);
  }

  // Print environment variables and CSV header
  ev.DisplayEnvVars();
  if (ev.outputToCsv)
  {
    printf("Test#,Transfer#,NumBytes,Src,Exe,Dst,CUs,BW(GB/s),Time(ms),SrcAddr,DstAddr\n");
  }

  int testNum = 0;
  char line[MAX_LINE_LEN];
  while(fgets(line, MAX_LINE_LEN, fp))
  {
    // Check if line is a comment to be echoed to output (starts with ##)
    if (!ev.outputToCsv && line[0] == '#' && line[1] == '#') printf("%s", line);

    // Parse set of parallel Transfers to execute
    std::vector<Transfer> transfers;
    ParseTransfers(line, ev.numCpuDevices, ev.numGpuDevices, transfers);
    if (transfers.empty()) continue;

    // If the number of bytes is specified, use it
    if (numBytesPerTransfer != 0)
    {
      size_t N = numBytesPerTransfer / sizeof(float);
      ExecuteTransfers(ev, ++testNum, N, transfers);
    }
    else
    {
      // Otherwise generate a range of values
      for (int N = 256; N <= (1<<27); N *= 2)
      {
        int delta = std::max(1, N / ev.samplingFactor);
        int curr = N;
        while (curr < N * 2)
        {
          ExecuteTransfers(ev, ++testNum, curr, transfers);
          curr += delta;
        }
      }
    }
  }
  fclose(fp);

  return 0;
}

void ExecuteTransfers(EnvVars const& ev,
                      int const testNum,
                      size_t const N,
                      std::vector<Transfer>& transfers,
                      bool verbose,
                      double* totalBandwidthCpu)
{
  int const initOffset = ev.byteOffset / sizeof(float);

  // Map transfers by executor
  TransferMap transferMap;
  for (int i = 0; i < transfers.size(); i++)
  {
    Transfer& transfer = transfers[i];
    transfer.transferIndex = i;
    Executor executor(transfer.exeType, transfer.exeIndex);
    ExecutorInfo& executorInfo = transferMap[executor];
    executorInfo.transfers.push_back(&transfer);
  }

  // Loop over each executor and prepare sub-executors
  std::map<int, Transfer*> transferList;
  for (auto& exeInfoPair : transferMap)
  {
    Executor const& executor = exeInfoPair.first;
    ExecutorInfo& exeInfo    = exeInfoPair.second;
    ExeType const exeType    = executor.first;
    int     const exeIndex   = RemappedIndex(executor.second, IsCpuType(exeType));

    exeInfo.totalTime = 0.0;
    exeInfo.totalSubExecs = 0;

    // Loop over each transfer this executor is involved in
    for (Transfer* transfer : exeInfo.transfers)
    {
      // Determine how many bytes to copy for this Transfer (use custom if pre-specified)
      transfer->numBytesActual = (transfer->numBytes ? transfer->numBytes : N * sizeof(float));

      // Allocate source memory
      transfer->srcMem.resize(transfer->numSrcs);
      for (int iSrc = 0; iSrc < transfer->numSrcs; ++iSrc)
      {
        MemType const& srcType  = transfer->srcType[iSrc];
        int     const  srcIndex    = RemappedIndex(transfer->srcIndex[iSrc], IsCpuType(srcType));

        // Ensure executing GPU can access source memory
        if (IsGpuType(exeType) && IsGpuType(srcType) && srcIndex != exeIndex)
          EnablePeerAccess(exeIndex, srcIndex);

        AllocateMemory(srcType, srcIndex, transfer->numBytesActual + ev.byteOffset, (void**)&transfer->srcMem[iSrc]);
      }

      // Allocate destination memory
      transfer->dstMem.resize(transfer->numDsts);
      for (int iDst = 0; iDst < transfer->numDsts; ++iDst)
      {
        MemType const& dstType  = transfer->dstType[iDst];
        int     const  dstIndex    = RemappedIndex(transfer->dstIndex[iDst], IsCpuType(dstType));

        // Ensure executing GPU can access destination memory
        if (IsGpuType(exeType) && IsGpuType(dstType) && dstIndex != exeIndex)
          EnablePeerAccess(exeIndex, dstIndex);

        AllocateMemory(dstType, dstIndex, transfer->numBytesActual + ev.byteOffset, (void**)&transfer->dstMem[iDst]);
      }

      exeInfo.totalSubExecs += transfer->numSubExecs;
      transferList[transfer->transferIndex] = transfer;
    }

    // Prepare additional requirement for GPU-based executors
    if (IsGpuType(exeType))
    {
      HIP_CALL(hipSetDevice(exeIndex));

      // Single-stream is only supported for GFX-based executors
      int const numStreamsToUse = (exeType == EXE_GPU_DMA || !ev.useSingleStream) ? exeInfo.transfers.size() : 1;
      exeInfo.streams.resize(numStreamsToUse);
      exeInfo.startEvents.resize(numStreamsToUse);
      exeInfo.stopEvents.resize(numStreamsToUse);
      for (int i = 0; i < numStreamsToUse; ++i)
      {
        if (ev.cuMask.size())
        {
#if !defined(__NVCC__)
          HIP_CALL(hipExtStreamCreateWithCUMask(&exeInfo.streams[i], ev.cuMask.size(), ev.cuMask.data()));
#endif
        }
        else
        {
          HIP_CALL(hipStreamCreate(&exeInfo.streams[i]));
        }
        HIP_CALL(hipEventCreate(&exeInfo.startEvents[i]));
        HIP_CALL(hipEventCreate(&exeInfo.stopEvents[i]));
      }

      if (exeType == EXE_GPU_GFX)
      {
        // Allocate one contiguous chunk of GPU memory for threadblock parameters
        // This allows support for executing one transfer per stream, or all transfers in a single stream
#if !defined(__NVCC__)
        AllocateMemory(MEM_GPU, exeIndex, exeInfo.totalSubExecs * sizeof(SubExecParam),
                       (void**)&exeInfo.subExecParamGpu);
#else
        AllocateMemory(MEM_CPU, exeIndex, exeInfo.totalSubExecs * sizeof(SubExecParam),
                       (void**)&exeInfo.subExecParamGpu);
#endif
      }
    }
  }

  if (verbose && !ev.outputToCsv) printf("Test %d:\n", testNum);

  // Prepare input memory and block parameters for current N
  bool isSrcCorrect = true;
  for (auto& exeInfoPair : transferMap)
  {
    Executor const& executor = exeInfoPair.first;
    ExecutorInfo& exeInfo    = exeInfoPair.second;
    ExeType const exeType    = executor.first;
    int     const exeIndex   = RemappedIndex(executor.second, IsCpuType(exeType));

    exeInfo.totalBytes = 0;
    for (int i = 0; i < exeInfo.transfers.size(); ++i)
    {
      // Prepare subarrays each threadblock works on and fill src memory with patterned data
      Transfer* transfer = exeInfo.transfers[i];
      transfer->PrepareSubExecParams(ev);
      isSrcCorrect &= transfer->PrepareSrc(ev);
      exeInfo.totalBytes += transfer->numBytesActual;
    }

    // Copy block parameters to GPU for GPU executors
    if (exeType == EXE_GPU_GFX)
    {
      std::vector<SubExecParam> tempSubExecParam;

      if (!ev.useSingleStream || (ev.blockOrder == ORDER_SEQUENTIAL))
      {
        // Assign Transfers to sequentual threadblocks
        int transferOffset = 0;
        for (Transfer* transfer : exeInfo.transfers)
        {
          transfer->subExecParamGpuPtr = exeInfo.subExecParamGpu + transferOffset;

          transfer->subExecIdx.clear();
          for (int subExecIdx = 0; subExecIdx < transfer->subExecParam.size(); subExecIdx++)
          {
            transfer->subExecIdx.push_back(transferOffset + subExecIdx);
            tempSubExecParam.push_back(transfer->subExecParam[subExecIdx]);
          }
          transferOffset += transfer->numSubExecs;
        }
      }
      else if (ev.blockOrder == ORDER_INTERLEAVED)
      {
        // Interleave threadblocks of different Transfers
        exeInfo.transfers[0]->subExecParamGpuPtr = exeInfo.subExecParamGpu;
        for (int subExecIdx = 0; tempSubExecParam.size() < exeInfo.totalSubExecs; ++subExecIdx)
        {
          for (Transfer* transfer : exeInfo.transfers)
          {
            if (subExecIdx < transfer->numSubExecs)
            {
              transfer->subExecIdx.push_back(tempSubExecParam.size());
              tempSubExecParam.push_back(transfer->subExecParam[subExecIdx]);
            }
          }
        }
      }
      else if (ev.blockOrder == ORDER_RANDOM)
      {
        std::vector<std::pair<int,int>> indices;
        exeInfo.transfers[0]->subExecParamGpuPtr = exeInfo.subExecParamGpu;

        // Build up a list of (transfer,subExecParam) indices, then randomly sort them
        for (int i = 0; i < exeInfo.transfers.size(); i++)
        {
          Transfer* transfer = exeInfo.transfers[i];
          for (int subExecIdx = 0; subExecIdx < transfer->numSubExecs; subExecIdx++)
            indices.push_back(std::make_pair(i, subExecIdx));
        }
        std::shuffle(indices.begin(), indices.end(), *ev.generator);

        // Build randomized threadblock list
        for (auto p : indices)
        {
          Transfer* transfer = exeInfo.transfers[p.first];
          transfer->subExecIdx.push_back(tempSubExecParam.size());
          tempSubExecParam.push_back(transfer->subExecParam[p.second]);
        }
      }

      HIP_CALL(hipSetDevice(exeIndex));
      HIP_CALL(hipMemcpy(exeInfo.subExecParamGpu,
                         tempSubExecParam.data(),
                         tempSubExecParam.size() * sizeof(SubExecParam),
                         hipMemcpyDefault));
      HIP_CALL(hipDeviceSynchronize());
    }
  }

  // Launch kernels (warmup iterations are not counted)
  double totalCpuTime = 0;
  size_t numTimedIterations = 0;
  std::stack<std::thread> threads;
  for (int iteration = -ev.numWarmups; isSrcCorrect; iteration++)
  {
    if (ev.numIterations > 0 && iteration    >= ev.numIterations) break;
    if (ev.numIterations < 0 && totalCpuTime > -ev.numIterations) break;

    // Pause before starting first timed iteration in interactive mode
    if (verbose && ev.useInteractive && iteration == 0)
    {
      printf("Memory prepared:\n");

      for (Transfer& transfer : transfers)
      {
        printf("Transfer %03d:\n", transfer.transferIndex);
        for (int iSrc = 0; iSrc < transfer.numSrcs; ++iSrc)
          printf("  SRC %0d: %p\n", iSrc, transfer.srcMem[iSrc]);
        for (int iDst = 0; iDst < transfer.numDsts; ++iDst)
          printf("  DST %0d: %p\n", iDst, transfer.dstMem[iDst]);
      }
      printf("Hit <Enter> to continue: ");
      if (scanf("%*c") != 0)
      {
        printf("[ERROR] Unexpected input\n");
        exit(1);
      }
      printf("\n");
    }

    // Start CPU timing for this iteration
    auto cpuStart = std::chrono::high_resolution_clock::now();

    // Execute all Transfers in parallel
    for (auto& exeInfoPair : transferMap)
    {
      ExecutorInfo& exeInfo = exeInfoPair.second;
      ExeType       exeType = exeInfoPair.first.first;
      int const numTransfersToRun = (exeType == EXE_GPU_GFX && ev.useSingleStream) ? 1 : exeInfo.transfers.size();

      for (int i = 0; i < numTransfersToRun; ++i)
        threads.push(std::thread(RunTransfer, std::ref(ev), iteration, std::ref(exeInfo), i));
    }

    // Wait for all threads to finish
    int const numTransfers = threads.size();
    for (int i = 0; i < numTransfers; i++)
    {
      threads.top().join();
      threads.pop();
    }

    // Stop CPU timing for this iteration
    auto cpuDelta = std::chrono::high_resolution_clock::now() - cpuStart;
    double deltaSec = std::chrono::duration_cast<std::chrono::duration<double>>(cpuDelta).count();

    if (ev.alwaysValidate)
    {
      for (auto transferPair : transferList)
      {
        Transfer* transfer = transferPair.second;
        transfer->ValidateDst(ev);
      }
    }

    if (iteration >= 0)
    {
      ++numTimedIterations;
      totalCpuTime += deltaSec;
    }
  }

  // Pause for interactive mode
  if (verbose && isSrcCorrect && ev.useInteractive)
  {
    printf("Transfers complete. Hit <Enter> to continue: ");
    if (scanf("%*c") != 0)
    {
      printf("[ERROR] Unexpected input\n");
      exit(1);
    }
    printf("\n");
  }

  // Validate that each transfer has transferred correctly
  size_t totalBytesTransferred = 0;
  int const numTransfers = transferList.size();
  for (auto transferPair : transferList)
  {
    Transfer* transfer = transferPair.second;
    transfer->ValidateDst(ev);
    totalBytesTransferred += transfer->numBytesActual;
  }

  // Report timings
  totalCpuTime = totalCpuTime / (1.0 * numTimedIterations) * 1000;
  double totalBandwidthGbs = (totalBytesTransferred / 1.0E6) / totalCpuTime;
  if (totalBandwidthCpu) *totalBandwidthCpu = totalBandwidthGbs;

  double maxGpuTime = 0;

  if (!isSrcCorrect) goto cleanup;
  if (ev.useSingleStream)
  {
    for (auto& exeInfoPair : transferMap)
    {
      ExecutorInfo  exeInfo  = exeInfoPair.second;
      ExeType const exeType  = exeInfoPair.first.first;
      int     const exeIndex = exeInfoPair.first.second;

      // Compute total time for non GPU executors
      if (exeType != EXE_GPU_GFX)
      {
        exeInfo.totalTime = 0;
        for (auto const& transfer : exeInfo.transfers)
          exeInfo.totalTime = std::max(exeInfo.totalTime, transfer->transferTime);
      }

      double exeDurationMsec = exeInfo.totalTime / (1.0 * numTimedIterations);
      double exeBandwidthGbs = (exeInfo.totalBytes / 1.0E9) / exeDurationMsec * 1000.0f;
      maxGpuTime = std::max(maxGpuTime, exeDurationMsec);

      if (verbose && !ev.outputToCsv)
      {
        printf(" Executor: %3s %02d | %7.3f GB/s | %8.3f ms | %12lu bytes\n",
               ExeTypeName[exeType], exeIndex, exeBandwidthGbs, exeDurationMsec, exeInfo.totalBytes);
      }

      int totalCUs = 0;
      for (auto const& transfer : exeInfo.transfers)
      {
        double transferDurationMsec = transfer->transferTime / (1.0 * numTimedIterations);
        double transferBandwidthGbs = (transfer->numBytesActual / 1.0E9) / transferDurationMsec * 1000.0f;
        totalCUs += transfer->numSubExecs;

        if (!verbose) continue;
        if (!ev.outputToCsv)
        {
          printf("     Transfer %02d  | %7.3f GB/s | %8.3f ms | %12lu bytes | %s -> %s%02d:%03d -> %s\n",
                 transfer->transferIndex,
                 transferBandwidthGbs,
                 transferDurationMsec,
                 transfer->numBytesActual,
                 transfer->SrcToStr().c_str(),
                 ExeTypeName[transfer->exeType], transfer->exeIndex,
                 transfer->numSubExecs,
                 transfer->DstToStr().c_str());

          if (ev.showIterations)
          {
            std::set<std::pair<double, int>> times;
            double stdDevTime = 0;
            double stdDevBw = 0;
            for (int i = 0; i < numTimedIterations; i++)
            {
              times.insert(std::make_pair(transfer->perIterationTime[i], i+1));
              double const varTime = fabs(transferDurationMsec - transfer->perIterationTime[i]);
              stdDevTime += varTime * varTime;

              double iterBandwidthGbs = (transfer->numBytesActual / 1.0E9) / transfer->perIterationTime[i] * 1000.0f;
              double const varBw = fabs(iterBandwidthGbs - transferBandwidthGbs);
              stdDevBw += varBw * varBw;
            }
            stdDevTime = sqrt(stdDevTime / numTimedIterations);
            stdDevBw = sqrt(stdDevBw / numTimedIterations);

            for (auto t : times)
            {
              double iterDurationMsec = t.first;
              double iterBandwidthGbs = (transfer->numBytesActual / 1.0E9) / iterDurationMsec * 1000.0f;
              printf("      Iter %03d    | %7.3f GB/s | %8.3f ms |", t.second, iterBandwidthGbs, iterDurationMsec);

              std::set<int> usedXccs;
              if (t.second - 1 < transfer->perIterationCUs.size())
              {
                printf(" CUs:");
                for (auto x : transfer->perIterationCUs[t.second - 1])
                {
                  printf(" %02d:%02d", x.first, x.second);
                  usedXccs.insert(x.first);
                }
              }
              printf(" XCCs:");
              for (auto x : usedXccs)
                printf(" %02d", x);
              printf("\n");
            }
            printf("      StandardDev | %7.3f GB/s | %8.3f ms |\n", stdDevBw, stdDevTime);
          }
        }
        else
        {
          printf("%d,%d,%lu,%s,%c%02d,%s,%d,%.3f,%.3f,%s,%s\n",
                 testNum, transfer->transferIndex, transfer->numBytesActual,
                 transfer->SrcToStr().c_str(),
                 MemTypeStr[transfer->exeType], transfer->exeIndex,
                 transfer->DstToStr().c_str(),
                 transfer->numSubExecs,
                 transferBandwidthGbs, transferDurationMsec,
                 PtrVectorToStr(transfer->srcMem, initOffset).c_str(),
                 PtrVectorToStr(transfer->dstMem, initOffset).c_str());
        }
      }

      if (verbose && ev.outputToCsv)
      {
        printf("%d,ALL,%lu,ALL,%c%02d,ALL,%d,%.3f,%.3f,ALL,ALL\n",
               testNum, totalBytesTransferred,
               MemTypeStr[exeType], exeIndex, totalCUs,
               exeBandwidthGbs, exeDurationMsec);
      }
    }
  }
  else
  {
    for (auto const& transferPair : transferList)
    {
      Transfer* transfer = transferPair.second;
      double transferDurationMsec = transfer->transferTime / (1.0 * numTimedIterations);
      double transferBandwidthGbs = (transfer->numBytesActual / 1.0E9) / transferDurationMsec * 1000.0f;
      maxGpuTime = std::max(maxGpuTime, transferDurationMsec);
      if (!verbose) continue;
      if (!ev.outputToCsv)
      {
        printf(" Transfer %02d      | %7.3f GB/s | %8.3f ms | %12lu bytes | %s -> %s%02d:%03d -> %s\n",
               transfer->transferIndex,
               transferBandwidthGbs, transferDurationMsec,
               transfer->numBytesActual,
               transfer->SrcToStr().c_str(),
               ExeTypeName[transfer->exeType], transfer->exeIndex,
               transfer->numSubExecs,
               transfer->DstToStr().c_str());

        if (ev.showIterations)
        {
            std::set<std::pair<double, int>> times;
            double stdDevTime = 0;
            double stdDevBw = 0;
            for (int i = 0; i < numTimedIterations; i++)
            {
              times.insert(std::make_pair(transfer->perIterationTime[i], i+1));
              double const varTime = fabs(transferDurationMsec - transfer->perIterationTime[i]);
              stdDevTime += varTime * varTime;

              double iterBandwidthGbs = (transfer->numBytesActual / 1.0E9) / transfer->perIterationTime[i] * 1000.0f;
              double const varBw = fabs(iterBandwidthGbs - transferBandwidthGbs);
              stdDevBw += varBw * varBw;
            }
            stdDevTime = sqrt(stdDevTime / numTimedIterations);
            stdDevBw = sqrt(stdDevBw / numTimedIterations);

            for (auto t : times)
            {
              double iterDurationMsec = t.first;
              double iterBandwidthGbs = (transfer->numBytesActual / 1.0E9) / iterDurationMsec * 1000.0f;
              printf("      Iter %03d    | %7.3f GB/s | %8.3f ms |", t.second, iterBandwidthGbs, iterDurationMsec);
              std::set<int> usedXccs;
              if (t.second - 1 < transfer->perIterationCUs.size())
              {
                printf(" CUs:");
                for (auto x : transfer->perIterationCUs[t.second - 1])
                {
                  printf(" %02d:%02d", x.first, x.second);
                  usedXccs.insert(x.first);
                }
              }
              printf(" XCCs:");
              for (auto x : usedXccs)
                printf(" %d", x);
              printf("\n");
            }
            printf("      StandardDev | %7.3f GB/s | %8.3f ms |\n", stdDevBw, stdDevTime);
        }
      }
      else
      {
        printf("%d,%d,%lu,%s,%s%02d,%s,%d,%.3f,%.3f,%s,%s\n",
               testNum, transfer->transferIndex, transfer->numBytesActual,
               transfer->SrcToStr().c_str(),
               ExeTypeName[transfer->exeType], transfer->exeIndex,
               transfer->DstToStr().c_str(),
               transfer->numSubExecs,
               transferBandwidthGbs, transferDurationMsec,
               PtrVectorToStr(transfer->srcMem, initOffset).c_str(),
               PtrVectorToStr(transfer->dstMem, initOffset).c_str());
      }
    }
  }

  // Display aggregate statistics
  if (verbose)
  {
    if (!ev.outputToCsv)
    {
      printf(" Aggregate (CPU)  | %7.3f GB/s | %8.3f ms | %12lu bytes | Overhead: %.3f ms\n",
             totalBandwidthGbs, totalCpuTime, totalBytesTransferred, totalCpuTime - maxGpuTime);
    }
    else
    {
      printf("%d,ALL,%lu,ALL,ALL,ALL,ALL,%.3f,%.3f,ALL,ALL\n",
             testNum, totalBytesTransferred, totalBandwidthGbs, totalCpuTime);
    }
  }

  // Release GPU memory
cleanup:
  for (auto exeInfoPair : transferMap)
  {
    ExecutorInfo& exeInfo  = exeInfoPair.second;
    ExeType const exeType  = exeInfoPair.first.first;
    int     const exeIndex = RemappedIndex(exeInfoPair.first.second, IsCpuType(exeType));

    for (auto& transfer : exeInfo.transfers)
    {
      for (int iSrc = 0; iSrc < transfer->numSrcs; ++iSrc)
      {
        MemType const& srcType = transfer->srcType[iSrc];
        DeallocateMemory(srcType, transfer->srcMem[iSrc], transfer->numBytesActual + ev.byteOffset);
      }
      for (int iDst = 0; iDst < transfer->numDsts; ++iDst)
      {
        MemType const& dstType = transfer->dstType[iDst];
        DeallocateMemory(dstType, transfer->dstMem[iDst], transfer->numBytesActual + ev.byteOffset);
      }
      transfer->subExecParam.clear();
    }

    if (IsGpuType(exeType))
    {
      int const numStreams = (int)exeInfo.streams.size();
      for (int i = 0; i < numStreams; ++i)
      {
        HIP_CALL(hipEventDestroy(exeInfo.startEvents[i]));
        HIP_CALL(hipEventDestroy(exeInfo.stopEvents[i]));
        HIP_CALL(hipStreamDestroy(exeInfo.streams[i]));
      }

      if (exeType == EXE_GPU_GFX)
      {
#if !defined(__NVCC__)
        DeallocateMemory(MEM_GPU, exeInfo.subExecParamGpu);
#else
        DeallocateMemory(MEM_CPU, exeInfo.subExecParamGpu);
#endif
      }
    }
  }
}

void DisplayUsage(char const* cmdName)
{
  printf("TransferBench v%s\n", TB_VERSION);
  printf("========================================\n");

  if (numa_available() == -1)
  {
    printf("[ERROR] NUMA library not supported. Check to see if libnuma has been installed on this system\n");
    exit(1);
  }
  int numGpuDevices;
  HIP_CALL(hipGetDeviceCount(&numGpuDevices));
  int const numCpuDevices = numa_num_configured_nodes();

  printf("Usage: %s config <N>\n", cmdName);
  printf("  config: Either:\n");
  printf("          - Filename of configFile containing Transfers to execute (see example.cfg for format)\n");
  printf("          - Name of preset config:\n");
  printf("              p2p          - Peer-to-peer benchmark tests\n");
  printf("              sweep/rsweep - Sweep/random sweep across possible sets of Transfers\n");
  printf("                             - 3rd optional arg: # GPU SubExecs per Transfer\n");
  printf("                             - 4th optional arg: # CPU SubExecs per Transfer\n");
  printf("              scaling      - GPU SubExec scaling copy test\n");
  printf("                             - 3th optional arg: Max # of SubExecs to use\n");
  printf("                             - 4rd optional arg: GPU index to use as executor\n");
  printf("              a2a          - GPU All-To-All benchmark\n");
  printf("                             - 3rd optional arg: # of SubExecs to use\n");
  printf("              cmdline      - Read Transfers from command line arguments (after N)\n");
  printf("  N     : (Optional) Number of bytes to copy per Transfer.\n");
  printf("          If not specified, defaults to %lu bytes. Must be a multiple of 4 bytes\n",
         DEFAULT_BYTES_PER_TRANSFER);
  printf("          If 0 is specified, a range of Ns will be benchmarked\n");
  printf("          May append a suffix ('K', 'M', 'G') for kilobytes / megabytes / gigabytes\n");
  printf("\n");

  EnvVars::DisplayUsage();
}

int RemappedIndex(int const origIdx, bool const isCpuType)
{
  static std::vector<int> remappingCpu;
  static std::vector<int> remappingGpu;

  // Build CPU remapping on first use
  // Skip numa nodes that are not configured
  if (remappingCpu.empty())
  {
    for (int node = 0; node <= numa_max_node(); node++)
      if (numa_bitmask_isbitset(numa_get_mems_allowed(), node))
        remappingCpu.push_back(node);
  }

  // Build remappingGpu on first use
  if (remappingGpu.empty())
  {
    int numGpuDevices;
    HIP_CALL(hipGetDeviceCount(&numGpuDevices));
    remappingGpu.resize(numGpuDevices);

    int const usePcieIndexing = getenv("USE_PCIE_INDEX") ? atoi(getenv("USE_PCIE_INDEX")) : 0;
    if (!usePcieIndexing)
    {
      // For HIP-based indexing no remappingGpu is necessary
      for (int i = 0; i < numGpuDevices; ++i)
        remappingGpu[i] = i;
    }
    else
    {
      // Collect PCIe address for each GPU
      std::vector<std::pair<std::string, int>> mapping;
      char pciBusId[20];
      for (int i = 0; i < numGpuDevices; ++i)
      {
        HIP_CALL(hipDeviceGetPCIBusId(pciBusId, 20, i));
        mapping.push_back(std::make_pair(pciBusId, i));
      }
      // Sort GPUs by PCIe address then use that as mapping
      std::sort(mapping.begin(), mapping.end());
      for (int i = 0; i < numGpuDevices; ++i)
        remappingGpu[i] = mapping[i].second;
    }
  }
  return isCpuType ? remappingCpu[origIdx] : remappingGpu[origIdx];
}

void DisplayTopology(bool const outputToCsv)
{

  int numCpuDevices = numa_num_configured_nodes();
  int numGpuDevices;
  HIP_CALL(hipGetDeviceCount(&numGpuDevices));

  if (outputToCsv)
  {
    printf("NumCpus,%d\n", numCpuDevices);
    printf("NumGpus,%d\n", numGpuDevices);
  }
  else
  {
    printf("\nDetected topology: %d configured CPU NUMA node(s) [%d total]   %d GPU device(s)\n",
           numa_num_configured_nodes(), numa_max_node() + 1, numGpuDevices);
  }

  // Print out detected CPU topology
  if (outputToCsv)
  {
    printf("NUMA");
    for (int j = 0; j < numCpuDevices; j++)
      printf(",NUMA%02d", j);
    printf(",# CPUs,ClosestGPUs,ActualNode\n");
  }
  else
  {
    printf("            |");
    for (int j = 0; j < numCpuDevices; j++)
      printf("NUMA %02d|", j);
    printf(" #Cpus | Closest GPU(s)\n");

    printf("------------+");
    for (int j = 0; j <= numCpuDevices; j++)
      printf("-------+");
    printf("---------------\n");
  }

  for (int i = 0; i < numCpuDevices; i++)
  {
    int nodeI = RemappedIndex(i, true);
    printf("NUMA %02d (%02d)%s", i, nodeI, outputToCsv ? "," : "|");
    for (int j = 0; j < numCpuDevices; j++)
    {
      int nodeJ = RemappedIndex(j, true);
      int numaDist = numa_distance(nodeI, nodeJ);
      if (outputToCsv)
        printf("%d,", numaDist);
      else
        printf(" %5d |", numaDist);
    }

    int numCpus = 0;
    for (int j = 0; j < numa_num_configured_cpus(); j++)
      if (numa_node_of_cpu(j) == nodeI) numCpus++;
    if (outputToCsv)
      printf("%d,", numCpus);
    else
      printf(" %5d | ", numCpus);

#if !defined(__NVCC__)
    bool isFirst = true;
    for (int j = 0; j < numGpuDevices; j++)
    {
      if (GetClosestNumaNode(RemappedIndex(j, false)) == i)
      {
        if (isFirst) isFirst = false;
        else printf(",");
        printf("%d", j);
      }
    }
#endif
    printf("\n");
  }
  printf("\n");

#if defined(__NVCC__)
  // No further topology detection done for NVIDIA platforms
  return;
#endif

  // Print out detected GPU topology
  if (outputToCsv)
  {
    printf("GPU");
    for (int j = 0; j < numGpuDevices; j++)
      printf(",GPU %02d", j);
    printf(",PCIe Bus ID,ClosestNUMA\n");
  }
  else
  {
    printf("        |");
    for (int j = 0; j < numGpuDevices; j++)
    {
      hipDeviceProp_t prop;
      HIP_CALL(hipGetDeviceProperties(&prop, j));
      std::string fullName = prop.gcnArchName;
      std::string archName = fullName.substr(0, fullName.find(':'));
      printf(" %6s |", archName.c_str());
    }
    printf("\n");
    printf("        |");
    for (int j = 0; j < numGpuDevices; j++)
      printf(" GPU %02d |", j);
    printf(" PCIe Bus ID  | #CUs | Closest NUMA\n");
    for (int j = 0; j <= numGpuDevices; j++)
      printf("--------+");
    printf("--------------+------+-------------\n");
  }

#if !defined(__NVCC__)
  char pciBusId[20];
  for (int i = 0; i < numGpuDevices; i++)
  {
    int const deviceIdx = RemappedIndex(i, false);
    printf("%sGPU %02d%s", outputToCsv ? "" : " ", i, outputToCsv ? "," : " |");
    for (int j = 0; j < numGpuDevices; j++)
    {
      if (i == j)
      {
        if (outputToCsv)
          printf("-,");
        else
          printf("    -   |");
      }
      else
      {
        uint32_t linkType, hopCount;
        HIP_CALL(hipExtGetLinkTypeAndHopCount(deviceIdx,
                                              RemappedIndex(j, false),
                                              &linkType, &hopCount));
        printf("%s%s-%d%s",
               outputToCsv ? "" : " ",
               linkType == HSA_AMD_LINK_INFO_TYPE_HYPERTRANSPORT ? "  HT" :
               linkType == HSA_AMD_LINK_INFO_TYPE_QPI            ? " QPI" :
               linkType == HSA_AMD_LINK_INFO_TYPE_PCIE           ? "PCIE" :
               linkType == HSA_AMD_LINK_INFO_TYPE_INFINBAND      ? "INFB" :
               linkType == HSA_AMD_LINK_INFO_TYPE_XGMI           ? "XGMI" : "????",
               hopCount, outputToCsv ? "," : " |");
      }
    }
    HIP_CALL(hipDeviceGetPCIBusId(pciBusId, 20, deviceIdx));

    int numDeviceCUs = 0;
    HIP_CALL(hipDeviceGetAttribute(&numDeviceCUs, hipDeviceAttributeMultiprocessorCount, deviceIdx));

    if (outputToCsv)
      printf("%s,%d,%d\n", pciBusId, numDeviceCUs, GetClosestNumaNode(deviceIdx));
    else
      printf(" %11s | %4d | %d\n", pciBusId, numDeviceCUs, GetClosestNumaNode(deviceIdx));
  }
#endif
}

void ParseMemType(std::string const& token, int const numCpus, int const numGpus,
                  std::vector<MemType>& memTypes, std::vector<int>& memIndices)
{
  char typeChar;
  int offset = 0, devIndex, inc;
  bool found = false;

  memTypes.clear();
  memIndices.clear();
  while (sscanf(token.c_str() + offset, " %c %d%n", &typeChar, &devIndex, &inc) == 2)
  {
    offset += inc;
    MemType memType = CharToMemType(typeChar);

    if (IsCpuType(memType) && (devIndex < 0 || devIndex >= numCpus))
    {
      printf("[ERROR] CPU index must be between 0 and %d (instead of %d)\n", numCpus-1, devIndex);
      exit(1);
    }
    if (IsGpuType(memType) && (devIndex < 0 || devIndex >= numGpus))
    {
      printf("[ERROR] GPU index must be between 0 and %d (instead of %d)\n", numGpus-1, devIndex);
      exit(1);
    }

    found = true;
    if (memType != MEM_NULL)
    {
      memTypes.push_back(memType);
      memIndices.push_back(devIndex);
    }
  }
  if (!found)
  {
    printf("[ERROR] Unable to parse memory type token %s.  Expected one of %s followed by an index\n",
           token.c_str(), MemTypeStr);
    exit(1);
  }
}

void ParseExeType(std::string const& token, int const numCpus, int const numGpus,
                  ExeType &exeType, int& exeIndex)
{
  char typeChar;
  if (sscanf(token.c_str(), " %c%d", &typeChar, &exeIndex) != 2)
  {
    printf("[ERROR] Unable to parse valid executor token (%s).  Exepected one of %s followed by an index\n",
           token.c_str(), ExeTypeStr);
    exit(1);
  }
  exeType = CharToExeType(typeChar);

  if (IsCpuType(exeType) && (exeIndex < 0 || exeIndex >= numCpus))
  {
    printf("[ERROR] CPU index must be between 0 and %d (instead of %d)\n", numCpus-1, exeIndex);
    exit(1);
  }
  if (IsGpuType(exeType) && (exeIndex < 0 || exeIndex >= numGpus))
  {
    printf("[ERROR] GPU index must be between 0 and %d (instead of %d)\n", numGpus-1, exeIndex);
    exit(1);
  }
}

// Helper function to parse a list of Transfer definitions
void ParseTransfers(char* line, int numCpus, int numGpus, std::vector<Transfer>& transfers)
{
  // Replace any round brackets or '->' with spaces,
  for (int i = 1; line[i]; i++)
    if (line[i] == '(' || line[i] == ')' || line[i] == '-' || line[i] == '>' ) line[i] = ' ';

  transfers.clear();

  int numTransfers = 0;
  std::istringstream iss(line);
  iss >> numTransfers;
  if (iss.fail()) return;

  std::string exeMem;
  std::string srcMem;
  std::string dstMem;

  // If numTransfers < 0, read 5-tuple (srcMem, exeMem, dstMem, #CUs, #Bytes)
  // otherwise read triples (srcMem, exeMem, dstMem)
  bool const advancedMode = (numTransfers < 0);
  numTransfers = abs(numTransfers);

  int numSubExecs;
  if (!advancedMode)
  {
    iss >> numSubExecs;
    if (numSubExecs <= 0 || iss.fail())
    {
      printf("Parsing error: Number of blocks to use (%d) must be greater than 0\n", numSubExecs);
      exit(1);
    }
  }

  size_t numBytes = 0;
  for (int i = 0; i < numTransfers; i++)
  {
    Transfer transfer;
    transfer.numBytes = 0;
    transfer.numBytesActual = 0;
    if (!advancedMode)
    {
      iss >> srcMem >> exeMem >> dstMem;
      if (iss.fail())
      {
        printf("Parsing error: Unable to read valid Transfer %d (SRC EXE DST) triplet\n", i+1);
        exit(1);
      }
    }
    else
    {
      std::string numBytesToken;
      iss >> srcMem >> exeMem >> dstMem >> numSubExecs >> numBytesToken;
      if (iss.fail())
      {
        printf("Parsing error: Unable to read valid Transfer %d (SRC EXE DST #CU #Bytes) tuple\n", i+1);
        exit(1);
      }
      if (sscanf(numBytesToken.c_str(), "%lu", &numBytes) != 1)
      {
        printf("Parsing error: '%s' is not a valid expression of numBytes for Transfer %d\n", numBytesToken.c_str(), i+1);
        exit(1);
      }
      char units = numBytesToken.back();
      switch (toupper(units))
      {
      case 'K': numBytes *= 1024; break;
      case 'M': numBytes *= 1024*1024; break;
      case 'G': numBytes *= 1024*1024*1024; break;
      }
    }

    ParseMemType(srcMem, numCpus, numGpus, transfer.srcType, transfer.srcIndex);
    ParseMemType(dstMem, numCpus, numGpus, transfer.dstType, transfer.dstIndex);
    ParseExeType(exeMem, numCpus, numGpus, transfer.exeType, transfer.exeIndex);

    transfer.numSrcs = (int)transfer.srcType.size();
    transfer.numDsts = (int)transfer.dstType.size();
    if (transfer.numSrcs == 0 && transfer.numDsts == 0)
    {
      printf("[ERROR] Transfer must have at least one src or dst\n");
      exit(1);
    }

    if (transfer.exeType == EXE_GPU_DMA && (transfer.numSrcs > 1 || transfer.numDsts > 1))
    {
      printf("[ERROR] GPU DMA executor can only be used for single source / single dst Transfers\n");
      exit(1);
    }

    transfer.numSubExecs = numSubExecs;
    transfer.numBytes = numBytes;
    transfers.push_back(transfer);
  }
}

void EnablePeerAccess(int const deviceId, int const peerDeviceId)
{
  int canAccess;
  HIP_CALL(hipDeviceCanAccessPeer(&canAccess, deviceId, peerDeviceId));
  if (!canAccess)
  {
    printf("[ERROR] Unable to enable peer access from GPU devices %d to %d\n", peerDeviceId, deviceId);
    exit(1);
  }
  HIP_CALL(hipSetDevice(deviceId));
  hipError_t error = hipDeviceEnablePeerAccess(peerDeviceId, 0);
  if (error != hipSuccess && error != hipErrorPeerAccessAlreadyEnabled)
  {
    printf("[ERROR] Unable to enable peer to peer access from %d to %d (%s)\n",
           deviceId, peerDeviceId, hipGetErrorString(error));
    exit(1);
  }
}

void AllocateMemory(MemType memType, int devIndex, size_t numBytes, void** memPtr)
{
  if (numBytes == 0)
  {
    printf("[ERROR] Unable to allocate 0 bytes\n");
    exit(1);
  }
  *memPtr = nullptr;
  if (IsCpuType(memType))
  {
    // Set numa policy prior to call to hipHostMalloc
    numa_set_preferred(devIndex);

    // Allocate host-pinned memory (should respect NUMA mem policy)
    if (memType == MEM_CPU_FINE)
    {
#if defined (__NVCC__)
      printf("[ERROR] Fine-grained CPU memory not supported on NVIDIA platform\n");
      exit(1);
#else
      HIP_CALL(hipHostMalloc((void **)memPtr, numBytes, hipHostMallocNumaUser));
#endif
    }
    else if (memType == MEM_CPU)
    {
#if defined (__NVCC__)
      if (hipHostMalloc((void **)memPtr, numBytes, 0) != hipSuccess)
#else
      if (hipHostMalloc((void **)memPtr, numBytes, hipHostMallocNumaUser | hipHostMallocNonCoherent) != hipSuccess)
#endif
      {
        printf("[ERROR] Unable to allocate non-coherent host memory on NUMA node %d\n", devIndex);
        exit(1);
      }
    }
    else if (memType == MEM_CPU_UNPINNED)
    {
      *memPtr = numa_alloc_onnode(numBytes, devIndex);
    }

    // Check that the allocated pages are actually on the correct NUMA node
    memset(*memPtr, 0, numBytes);
    CheckPages((char*)*memPtr, numBytes, devIndex);

    // Reset to default numa mem policy
    numa_set_preferred(-1);
  }
  else if (IsGpuType(memType))
  {
    if (memType == MEM_GPU)
    {
      // Allocate GPU memory on appropriate device
      HIP_CALL(hipSetDevice(devIndex));
      HIP_CALL(hipMalloc((void**)memPtr, numBytes));
    }
    else if (memType == MEM_GPU_FINE)
    {
#if defined (__NVCC__)
      printf("[ERROR] Fine-grained GPU memory not supported on NVIDIA platform\n");
      exit(1);
#else
      HIP_CALL(hipSetDevice(devIndex));

      // NOTE: hipDeviceMallocFinegrained will be replaced by hipDeviceMallocUncached eventually
      //       Until then, this workaround is required
      hipDeviceProp_t prop;
      HIP_CALL(hipGetDeviceProperties(&prop, 0));
      int flag = (prop.gcnArch / 10 == 94) ? 0x3 : hipDeviceMallocFinegrained;
      HIP_CALL(hipExtMallocWithFlags((void**)memPtr, numBytes, flag));
#endif
    }
    HIP_CALL(hipMemset(*memPtr, 0, numBytes));
    HIP_CALL(hipDeviceSynchronize());
  }
  else
  {
    printf("[ERROR] Unsupported memory type %d\n", memType);
    exit(1);
  }
}

void DeallocateMemory(MemType memType, void* memPtr, size_t const bytes)
{
  if (memType == MEM_CPU || memType == MEM_CPU_FINE)
  {
    if (memPtr == nullptr)
    {
      printf("[ERROR] Attempting to free null CPU pointer for %lu bytes.  Skipping hipHostFree\n", bytes);
      return;
    }
    HIP_CALL(hipHostFree(memPtr));
  }
  else if (memType == MEM_CPU_UNPINNED)
  {
    if (memPtr == nullptr)
    {
      printf("[ERROR] Attempting to free null unpinned CPU pointer for %lu bytes.  Skipping numa_free\n", bytes);
      return;
    }
    numa_free(memPtr, bytes);
  }
  else if (memType == MEM_GPU || memType == MEM_GPU_FINE)
  {
    if (memPtr == nullptr)
    {
      printf("[ERROR] Attempting to free null GPU pointer for %lu bytes. Skipping hipFree\n", bytes);
      return;
    }
    HIP_CALL(hipFree(memPtr));
  }
}

void CheckPages(char* array, size_t numBytes, int targetId)
{
  unsigned long const pageSize = getpagesize();
  unsigned long const numPages = (numBytes + pageSize - 1) / pageSize;

  std::vector<void *> pages(numPages);
  std::vector<int> status(numPages);

  pages[0] = array;
  for (int i = 1; i < numPages; i++)
  {
    pages[i] = (char*)pages[i-1] + pageSize;
  }

  long const retCode = move_pages(0, numPages, pages.data(), NULL, status.data(), 0);
  if (retCode)
  {
    printf("[ERROR] Unable to collect page info\n");
    exit(1);
  }

  size_t mistakeCount = 0;
  for (int i = 0; i < numPages; i++)
  {
    if (status[i] < 0)
    {
      printf("[ERROR] Unexpected page status %d for page %d\n", status[i], i);
      exit(1);
    }
    if (status[i] != targetId) mistakeCount++;
  }
  if (mistakeCount > 0)
  {
    printf("[ERROR] %lu out of %lu pages for memory allocation were not on NUMA node %d\n", mistakeCount, numPages, targetId);
    exit(1);
  }
}

uint32_t GetId(uint32_t hwId)
{
  // Based on instinct-mi200-cdna2-instruction-set-architecture.pdf
  int const shId = (hwId >> 12) &  1;
  int const cuId = (hwId >>  8) & 15;
  int const seId = (hwId >> 13) &  3;
  return (shId << 5) + (cuId << 2) + seId;
}

void RunTransfer(EnvVars const& ev, int const iteration,
                 ExecutorInfo& exeInfo, int const transferIdx)
{
  Transfer* transfer = exeInfo.transfers[transferIdx];

  if (transfer->exeType == EXE_GPU_GFX)
  {
    // Switch to executing GPU
    int const exeIndex = RemappedIndex(transfer->exeIndex, false);
    HIP_CALL(hipSetDevice(exeIndex));

    hipStream_t& stream     = exeInfo.streams[transferIdx];
    hipEvent_t&  startEvent = exeInfo.startEvents[transferIdx];
    hipEvent_t&  stopEvent  = exeInfo.stopEvents[transferIdx];

    // Figure out how many threadblocks to use.
    // In single stream mode, all the threadblocks for this GPU are launched
    // Otherwise, just launch the threadblocks associated with this single Transfer
    int const numBlocksToRun = ev.useSingleStream ? exeInfo.totalSubExecs : transfer->numSubExecs;
    int const numXCCs = (ev.useXccFilter ? ev.xccIdsPerDevice[exeIndex].size() : 1);

#if defined(__NVCC__)
    HIP_CALL(hipEventRecord(startEvent, stream));
    GpuKernelTable[ev.gpuKernel]<<<numBlocksToRun, ev.blockSize, ev.sharedMemBytes, stream>>>(transfer->subExecParamGpuPtr);
    HIP_CALL(hipEventRecord(stopEvent, stream));
#else
    hipExtLaunchKernelGGL(GpuKernelTable[ev.gpuKernel],
                          dim3(numXCCs, numBlocksToRun, 1),
                          dim3(ev.blockSize, 1, 1),
                          ev.sharedMemBytes, stream,
                          startEvent, stopEvent,
                          0, transfer->subExecParamGpuPtr);
#endif
    // Synchronize per iteration, unless in single sync mode, in which case
    // synchronize during last warmup / last actual iteration
    HIP_CALL(hipStreamSynchronize(stream));

    if (iteration >= 0)
    {
      // Record GPU timing
      float gpuDeltaMsec;
      HIP_CALL(hipEventElapsedTime(&gpuDeltaMsec, startEvent, stopEvent));

      if (ev.useSingleStream)
      {
        // Figure out individual timings for Transfers that were all launched together
        for (Transfer* currTransfer : exeInfo.transfers)
        {
          long long minStartCycle = std::numeric_limits<long long>::max();
          long long maxStopCycle  = std::numeric_limits<long long>::min();

          std::set<std::pair<int,int>> CUs;
          for (auto subExecIdx : currTransfer->subExecIdx)
          {
            minStartCycle = std::min(minStartCycle, exeInfo.subExecParamGpu[subExecIdx].startCycle);
            maxStopCycle  = std::max(maxStopCycle,  exeInfo.subExecParamGpu[subExecIdx].stopCycle);
            if (ev.showIterations)
              CUs.insert(std::make_pair(exeInfo.subExecParamGpu[subExecIdx].xccId,
                                        GetId(exeInfo.subExecParamGpu[subExecIdx].hwId)));
          }
          int const wallClockRate = ev.wallClockPerDeviceMhz[exeIndex];
          double iterationTimeMs = (maxStopCycle - minStartCycle) / (double)(wallClockRate);
          currTransfer->transferTime += iterationTimeMs;
          if (ev.showIterations)
          {
            currTransfer->perIterationTime.push_back(iterationTimeMs);
            currTransfer->perIterationCUs.push_back(CUs);
          }
        }
        exeInfo.totalTime += gpuDeltaMsec;
      }
      else
      {
        transfer->transferTime += gpuDeltaMsec;
        if (ev.showIterations)
        {
          transfer->perIterationTime.push_back(gpuDeltaMsec);
          std::set<std::pair<int,int>> CUs;
          for (int i = 0; i < transfer->numSubExecs; i++)
            CUs.insert(std::make_pair(transfer->subExecParamGpuPtr[i].xccId,
                                      GetId(transfer->subExecParamGpuPtr[i].hwId)));
          transfer->perIterationCUs.push_back(CUs);
        }
      }
    }
  }
  else if (transfer->exeType == EXE_GPU_DMA)
  {
    // Switch to executing GPU
    int const exeIndex = RemappedIndex(transfer->exeIndex, false);
    HIP_CALL(hipSetDevice(exeIndex));

    hipStream_t& stream     = exeInfo.streams[transferIdx];
    hipEvent_t&  startEvent = exeInfo.startEvents[transferIdx];
    hipEvent_t&  stopEvent  = exeInfo.stopEvents[transferIdx];

    HIP_CALL(hipEventRecord(startEvent, stream));
    if (transfer->numSrcs == 0 && transfer->numDsts == 1)
    {
      HIP_CALL(hipMemsetAsync(transfer->dstMem[0],
                              MEMSET_CHAR, transfer->numBytesActual, stream));
    }
    else if (transfer->numSrcs == 1 && transfer->numDsts == 1)
    {
      HIP_CALL(hipMemcpyAsync(transfer->dstMem[0], transfer->srcMem[0],
                              transfer->numBytesActual, hipMemcpyDefault,
                              stream));
    }
    HIP_CALL(hipEventRecord(stopEvent, stream));
    HIP_CALL(hipStreamSynchronize(stream));

    if (iteration >= 0)
    {
      // Record GPU timing
      float gpuDeltaMsec;
      HIP_CALL(hipEventElapsedTime(&gpuDeltaMsec, startEvent, stopEvent));
      transfer->transferTime += gpuDeltaMsec;
      if (ev.showIterations)
        transfer->perIterationTime.push_back(gpuDeltaMsec);
    }
  }
  else if (transfer->exeType == EXE_CPU) // CPU execution agent
  {
    // Force this thread and all child threads onto correct NUMA node
    int const exeIndex = RemappedIndex(transfer->exeIndex, true);
    if (numa_run_on_node(exeIndex))
    {
      printf("[ERROR] Unable to set CPU to NUMA node %d\n", exeIndex);
      exit(1);
    }

    std::vector<std::thread> childThreads;

    auto cpuStart = std::chrono::high_resolution_clock::now();

    // Launch each subExecutor in child-threads to perform memcopies
    for (int i = 0; i < transfer->numSubExecs; ++i)
      childThreads.push_back(std::thread(CpuReduceKernel, std::ref(transfer->subExecParam[i])));

    // Wait for child-threads to finish
    for (int i = 0; i < transfer->numSubExecs; ++i)
      childThreads[i].join();

    auto cpuDelta = std::chrono::high_resolution_clock::now() - cpuStart;

    // Record time if not a warmup iteration
    if (iteration >= 0)
    {
      double const delta = (std::chrono::duration_cast<std::chrono::duration<double>>(cpuDelta).count() * 1000.0);
      transfer->transferTime += delta;
      if (ev.showIterations)
        transfer->perIterationTime.push_back(delta);
    }
  }
}

void RunPeerToPeerBenchmarks(EnvVars const& ev, size_t N)
{
  ev.DisplayP2PBenchmarkEnvVars();

  char const separator = ev.outputToCsv ? ',' : ' ';
  printf("Bytes Per Direction%c%lu\n", separator, N * sizeof(float));

  // Collect the number of available CPUs/GPUs on this machine
  int const numCpus    = ev.numCpuDevices;
  int const numGpus    = ev.numGpuDevices;
  int const numDevices = numCpus + numGpus;

  // Enable peer to peer for each GPU
  for (int i = 0; i < numGpus; i++)
    for (int j = 0; j < numGpus; j++)
      if (i != j) EnablePeerAccess(i, j);

  // Perform unidirectional / bidirectional
  for (int isBidirectional = 0; isBidirectional <= 1; isBidirectional++)
  {
    if (ev.p2pMode == 1 && isBidirectional == 1 ||
        ev.p2pMode == 2 && isBidirectional == 0) continue;

    printf("%sdirectional copy peak bandwidth GB/s [%s read / %s write] (GPU-Executor: %s)\n", isBidirectional ? "Bi" : "Uni",
           ev.useRemoteRead ? "Remote" : "Local",
           ev.useRemoteRead ? "Local" : "Remote",
           ev.useDmaCopy    ? "DMA"   : "GFX");

    // Print header
    if (isBidirectional)
    {
      printf("%12s", "SRC\\DST");
    }
    else
    {
      if (ev.useRemoteRead)
        printf("%12s", "SRC\\EXE+DST");
      else
        printf("%12s", "SRC+EXE\\DST");
    }
    if (ev.outputToCsv) printf(",");
    for (int i = 0; i < numCpus; i++)
    {
      printf("%7s %02d", "CPU", i);
      if (ev.outputToCsv) printf(",");
    }
    if (numCpus > 0) printf("   ");
    for (int i = 0; i < numGpus; i++)
    {
      printf("%7s %02d", "GPU", i);
      if (ev.outputToCsv) printf(",");
    }
    printf("\n");

    double avgBwSum[2][2] = {};
    int    avgCount[2][2] = {};

    ExeType const gpuExeType = ev.useDmaCopy ? EXE_GPU_DMA : EXE_GPU_GFX;
    // Loop over all possible src/dst pairs
    for (int src = 0; src < numDevices; src++)
    {
      MemType const srcType  = (src < numCpus ? MEM_CPU : MEM_GPU);
      int     const srcIndex = (srcType == MEM_CPU ? src : src - numCpus);
      MemType const srcTypeActual = ((ev.useFineGrain && srcType == MEM_CPU) ? MEM_CPU_FINE :
                                     (ev.useFineGrain && srcType == MEM_GPU) ? MEM_GPU_FINE :
                                                                               srcType);
      std::vector<std::vector<double>> avgBandwidth(isBidirectional + 1);
      std::vector<std::vector<double>> minBandwidth(isBidirectional + 1);
      std::vector<std::vector<double>> maxBandwidth(isBidirectional + 1);
      std::vector<std::vector<double>> stdDev(isBidirectional + 1);

      if (src == numCpus && src != 0) printf("\n");
      for (int dst = 0; dst < numDevices; dst++)
      {
        MemType const dstType  = (dst < numCpus ? MEM_CPU : MEM_GPU);
        int     const dstIndex = (dstType == MEM_CPU ? dst : dst - numCpus);
        MemType const dstTypeActual = ((ev.useFineGrain && dstType == MEM_CPU) ? MEM_CPU_FINE :
                                       (ev.useFineGrain && dstType == MEM_GPU) ? MEM_GPU_FINE :
                                                                                 dstType);
        // Prepare Transfers
        std::vector<Transfer> transfers(isBidirectional + 1);

        // SRC -> DST
        transfers[0].numBytes = N * sizeof(float);
        transfers[0].srcType.push_back(srcTypeActual);
        transfers[0].dstType.push_back(dstTypeActual);
        transfers[0].srcIndex.push_back(srcIndex);
        transfers[0].dstIndex.push_back(dstIndex);
        transfers[0].numSrcs = transfers[0].numDsts = 1;
        transfers[0].exeType = IsGpuType(ev.useRemoteRead ? dstType : srcType) ? gpuExeType : EXE_CPU;
        transfers[0].exeIndex = (ev.useRemoteRead ? dstIndex : srcIndex);
        transfers[0].numSubExecs = IsGpuType(transfers[0].exeType) ? ev.numGpuSubExecs : ev.numCpuSubExecs;

        // DST -> SRC
        if (isBidirectional)
        {
          transfers[1].numBytes = N * sizeof(float);
          transfers[1].numSrcs = transfers[1].numDsts = 1;
          transfers[1].srcType.push_back(dstTypeActual);
          transfers[1].dstType.push_back(srcTypeActual);
          transfers[1].srcIndex.push_back(dstIndex);
          transfers[1].dstIndex.push_back(srcIndex);
          transfers[1].exeType = IsGpuType(ev.useRemoteRead ? srcType : dstType) ? gpuExeType : EXE_CPU;
          transfers[1].exeIndex = (ev.useRemoteRead ? srcIndex : dstIndex);
          transfers[1].numSubExecs = IsGpuType(transfers[1].exeType) ? ev.numGpuSubExecs : ev.numCpuSubExecs;
        }

        bool skipTest = false;

        // Abort if executing on NUMA node with no CPUs
        for (int i = 0; i <= isBidirectional; i++)
        {
          if (transfers[i].exeType == EXE_CPU && ev.numCpusPerNuma[transfers[i].exeIndex] == 0)
          {
            skipTest = true;
            break;
          }

#if defined(__NVCC__)
          // NVIDIA platform cannot access GPU memory directly from CPU executors
          if (transfers[i].exeType == EXE_CPU && (IsGpuType(srcType) || IsGpuType(dstType)))
          {
            skipTest = true;
            break;
          }
#endif
        }

        if (isBidirectional && srcType == dstType && srcIndex == dstIndex) skipTest = true;

        if (!skipTest)
        {
          ExecuteTransfers(ev, 0, N, transfers, false);

          for (int dir = 0; dir <= isBidirectional; dir++)
          {
            double const avgTime = transfers[dir].transferTime / ev.numIterations;
            double const avgBw   = (transfers[dir].numBytesActual / 1.0E9) / avgTime * 1000.0f;
            avgBandwidth[dir].push_back(avgBw);

            if (!(srcType == dstType && srcIndex == dstIndex))
            {
              avgBwSum[srcType][dstType] += avgBw;
              avgCount[srcType][dstType]++;
            }

            if (ev.showIterations)
            {
              double minTime = transfers[dir].perIterationTime[0];
              double maxTime = transfers[dir].perIterationTime[0];
              double varSum  = 0;
              for (int i = 0; i < transfers[dir].perIterationTime.size(); i++)
              {
                minTime = std::min(minTime, transfers[dir].perIterationTime[i]);
                maxTime = std::max(maxTime, transfers[dir].perIterationTime[i]);
                double const bw  = (transfers[dir].numBytesActual / 1.0E9) / transfers[dir].perIterationTime[i] * 1000.0f;
                double const delta = (avgBw - bw);
                varSum += delta * delta;
              }
              double const minBw = (transfers[dir].numBytesActual / 1.0E9) / maxTime * 1000.0f;
              double const maxBw = (transfers[dir].numBytesActual / 1.0E9) / minTime * 1000.0f;
              double const stdev = sqrt(varSum / transfers[dir].perIterationTime.size());
              minBandwidth[dir].push_back(minBw);
              maxBandwidth[dir].push_back(maxBw);
              stdDev[dir].push_back(stdev);
            }
          }
        }
        else
        {
          for (int dir = 0; dir <= isBidirectional; dir++)
          {
            avgBandwidth[dir].push_back(0);
            minBandwidth[dir].push_back(0);
            maxBandwidth[dir].push_back(0);
            stdDev[dir].push_back(-1.0);
          }
        }
      }

      for (int dir = 0; dir <= isBidirectional; dir++)
      {
        printf("%5s %02d %3s", (srcType == MEM_CPU) ? "CPU" : "GPU", srcIndex, dir ? "<- " : " ->");
        if (ev.outputToCsv) printf(",");

        for (int dst = 0; dst < numDevices; dst++)
        {
          if (dst == numCpus && dst != 0) printf("   ");
          double const avgBw = avgBandwidth[dir][dst];

          if (avgBw == 0.0)
            printf("%10s", "N/A");
          else
            printf("%10.2f", avgBw);
          if (ev.outputToCsv) printf(",");
        }
        printf("\n");

        if (ev.showIterations)
        {
          // minBw
          printf("%5s %02d %3s", (srcType == MEM_CPU) ? "CPU" : "GPU", srcIndex, "min");
          if (ev.outputToCsv) printf(",");
          for (int i = 0; i < numDevices; i++)
          {
            double const minBw = minBandwidth[dir][i];
            if (i == numCpus && i != 0) printf("   ");
            if (minBw == 0.0)
              printf("%10s", "N/A");
            else
              printf("%10.2f", minBw);
            if (ev.outputToCsv) printf(",");
          }
          printf("\n");

          // maxBw
          printf("%5s %02d %3s", (srcType == MEM_CPU) ? "CPU" : "GPU", srcIndex, "max");
          if (ev.outputToCsv) printf(",");
          for (int i = 0; i < numDevices; i++)
          {
            double const maxBw = maxBandwidth[dir][i];
            if (i == numCpus && i != 0) printf("   ");
            if (maxBw == 0.0)
              printf("%10s", "N/A");
            else
              printf("%10.2f", maxBw);
            if (ev.outputToCsv) printf(",");
          }
          printf("\n");

          // stddev
          printf("%5s %02d %3s", (srcType == MEM_CPU) ? "CPU" : "GPU", srcIndex, " sd");
          if (ev.outputToCsv) printf(",");
          for (int i = 0; i < numDevices; i++)
          {
            double const sd = stdDev[dir][i];
            if (i == numCpus && i != 0) printf("   ");
            if (sd == -1.0)
              printf("%10s", "N/A");
            else
              printf("%10.2f", sd);
            if (ev.outputToCsv) printf(",");
          }
          printf("\n");
        }
        fflush(stdout);
      }

      if (isBidirectional)
      {
        printf("%5s %02d %3s", (srcType == MEM_CPU) ? "CPU" : "GPU", srcIndex, "<->");
        if (ev.outputToCsv) printf(",");
        for (int dst = 0; dst < numDevices; dst++)
        {
          double const sumBw = avgBandwidth[0][dst] + avgBandwidth[1][dst];
          if (dst == numCpus && dst != 0) printf("   ");
          if (sumBw == 0.0)
            printf("%10s", "N/A");
          else
            printf("%10.2f", sumBw);
          if (ev.outputToCsv) printf(",");
        }
        printf("\n");
        if (src < numDevices - 1) printf("\n");
      }
    }

    if (!ev.outputToCsv)
    {
      printf("                         ");
      for (int srcType : {MEM_CPU, MEM_GPU})
        for (int dstType : {MEM_CPU, MEM_GPU})
          printf("  %cPU->%cPU", srcType == MEM_CPU ? 'C' : 'G', dstType == MEM_CPU ? 'C' : 'G');
      printf("\n");

      printf("Averages (During %s):",  isBidirectional ? " BiDir" : "UniDir");
      for (int srcType : {MEM_CPU, MEM_GPU})
        for (int dstType : {MEM_CPU, MEM_GPU})
        {
          if (avgCount[srcType][dstType])
            printf("%10.2f", avgBwSum[srcType][dstType] / avgCount[srcType][dstType]);
          else
            printf("%10s", "N/A");
        }
      printf("\n\n");
    }
  }
}

void RunScalingBenchmark(EnvVars const& ev, size_t N, int const exeIndex, int const maxSubExecs)
{
  ev.DisplayEnvVars();

  // Collect the number of available CPUs/GPUs on this machine
  int const numCpus    = ev.numCpuDevices;
  int const numGpus    = ev.numGpuDevices;
  int const numDevices = numCpus + numGpus;

  // Enable peer to peer for each GPU
  for (int i = 0; i < numGpus; i++)
    for (int j = 0; j < numGpus; j++)
      if (i != j) EnablePeerAccess(i, j);

  char separator = (ev.outputToCsv ? ',' : ' ');

  std::vector<Transfer> transfers(1);
  transfers[0].numBytes = N * sizeof(float);
  transfers[0].numSrcs  = 1;
  transfers[0].numDsts  = 1;
  transfers[0].exeType  = EXE_GPU_GFX;
  transfers[0].exeIndex = exeIndex;
  transfers[0].srcType.resize(1, MEM_GPU);
  transfers[0].dstType.resize(1, MEM_GPU);
  transfers[0].srcIndex.resize(1);
  transfers[0].dstIndex.resize(1);

  printf("GPU-GFX Scaling benchmark:\n");
  printf("==========================\n");
  printf("- Copying %lu bytes from GPU %d to other devices\n", transfers[0].numBytes, exeIndex);
  printf("- All numbers reported as GB/sec\n\n");

  printf("NumCUs");
  for (int i = 0; i < numDevices; i++)
    printf("%c  %s%02d     ", separator, i < numCpus ? "CPU" : "GPU", i < numCpus ? i : i - numCpus);
  printf("\n");

  std::vector<std::pair<double, int>> bestResult(numDevices);
  for (int numSubExec = 1; numSubExec <= maxSubExecs; numSubExec++)
  {
    transfers[0].numSubExecs = numSubExec;
    printf("%4d  ", numSubExec);

    for (int i = 0; i < numDevices; i++)
    {
      transfers[0].dstType[0]  = i < numCpus ? MEM_CPU : MEM_GPU;
      transfers[0].dstIndex[0] = i < numCpus ? i : i - numCpus;

      ExecuteTransfers(ev, 0, N, transfers, false);
      double transferDurationMsec = transfers[0].transferTime / (1.0 * ev.numIterations);
      double transferBandwidthGbs = (transfers[0].numBytesActual / 1.0E9) / transferDurationMsec * 1000.0f;
      printf("%c%7.2f     ", separator, transferBandwidthGbs);

      if (transferBandwidthGbs > bestResult[i].first)
      {
        bestResult[i].first  = transferBandwidthGbs;
        bestResult[i].second = numSubExec;
      }
    }
    printf("\n");
  }

  printf(" Best ");
  for (int i = 0; i < numDevices; i++)
  {
    printf("%c%7.2f(%3d)", separator, bestResult[i].first, bestResult[i].second);
  }
  printf("\n");
}

void RunAllToAllBenchmark(EnvVars const& ev, size_t const numBytesPerTransfer, int const numSubExecs)
{
  ev.DisplayA2AEnvVars();

  // Collect the number of GPU devices to use
  int const numGpus = ev.numGpuDevices;

  // Enable peer to peer for each GPU
  for (int i = 0; i < numGpus; i++)
    for (int j = 0; j < numGpus; j++)
      if (i != j) EnablePeerAccess(i, j);

  char separator = (ev.outputToCsv ? ',' : ' ');

  Transfer transfer;
  transfer.numBytes    = numBytesPerTransfer;
  transfer.numSubExecs = numSubExecs;
  transfer.numSrcs     = 1;
  transfer.numDsts     = 1;
  transfer.exeType     = EXE_GPU_GFX;
  transfer.srcType.resize(1, ev.useFineGrain ? MEM_GPU_FINE : MEM_GPU);
  transfer.dstType.resize(1, ev.useFineGrain ? MEM_GPU_FINE : MEM_GPU);
  transfer.srcIndex.resize(1);
  transfer.dstIndex.resize(1);

  std::vector<Transfer> transfers;
  for (int i = 0; i < numGpus; i++)
  {
    transfer.srcIndex[0] = i;
    for (int j = 0; j < numGpus; j++)
    {
      transfer.dstIndex[0] = j;
      transfer.exeIndex    = (ev.useRemoteRead ? j : i);

      if (ev.a2aDirect)
      {
#if !defined(__NVCC__)
        if (i == j) continue;

        uint32_t linkType, hopCount;
        HIP_CALL(hipExtGetLinkTypeAndHopCount(RemappedIndex(i, false),
                                              RemappedIndex(j, false),
                                              &linkType, &hopCount));
        if (hopCount != 1) continue;
#endif
      }
      transfers.push_back(transfer);
    }
  }

  printf("GPU-GFX All-To-All benchmark:\n");
  printf("==========================\n");
  printf("- Copying %lu bytes between %s pairs of GPUs using %d CUs (%lu Transfers)\n",
         numBytesPerTransfer, ev.a2aDirect ? "directly connected" : "all", numSubExecs, transfers.size());
  if (transfers.size() == 0) return;

  double totalBandwidthCpu = 0;
  ExecuteTransfers(ev, 0, numBytesPerTransfer / sizeof(float), transfers, true, &totalBandwidthCpu);

  printf("\nSummary:\n");
  printf("==========================================================\n");
  printf("SRC\\DST");
  for (int dst = 0; dst < numGpus; dst++)
    printf("%cGPU %02d   ", separator, dst);
  printf("   %cSTotal\n", separator);

  std::map<std::pair<int, int>, int> reIndex;
  for (int i = 0; i < transfers.size(); i++)
  {
    Transfer const& t = transfers[i];
    reIndex[std::make_pair(t.srcIndex[0], t.dstIndex[0])] = i;
  }

  double totalBandwidthGpu = 0.0;
  std::vector<double> colTotalBandwidth(numGpus+1, 0.0);
  for (int src = 0; src < numGpus; src++)
  {
    double rowTotalBandwidth = 0;
    printf("GPU %02d", src);
    for (int dst = 0; dst < numGpus; dst++)
    {
      if (reIndex.count(std::make_pair(src, dst)))
      {
        Transfer const& transfer = transfers[reIndex[std::make_pair(src,dst)]];
        double transferDurationMsec = transfer.transferTime / (1.0 * ev.numIterations);
        double transferBandwidthGbs = (transfer.numBytesActual / 1.0E9) / transferDurationMsec * 1000.0f;
        colTotalBandwidth[dst] += transferBandwidthGbs;
        rowTotalBandwidth += transferBandwidthGbs;
        totalBandwidthGpu += transferBandwidthGbs;
        printf("%c%7.2f  ", separator, transferBandwidthGbs);
      }
      else
      {
        printf("%c%7s  ", separator, "N/A");
      }
    }
    printf("   %c%7.2f\n", separator, rowTotalBandwidth);
    colTotalBandwidth[numGpus] += rowTotalBandwidth;
  }
  printf("\nRTotal");
  for (int dst = 0; dst < numGpus; dst++)
  {
    printf("%c%7.2f  ", separator, colTotalBandwidth[dst]);
  }
  printf("   %c%7.2f\n", separator, colTotalBandwidth[numGpus]);
  printf("\n");

  printf("Average   bandwidth (GPU Timed): %7.2f GB/s\n", totalBandwidthGpu / transfers.size());
  printf("Aggregate bandwidth (GPU Timed): %7.2f GB/s\n", totalBandwidthGpu);
  printf("Aggregate bandwidth (CPU Timed): %7.2f GB/s\n", totalBandwidthCpu);
}

void Transfer::PrepareSubExecParams(EnvVars const& ev)
{
  // Each subExecutor needs to know src/dst pointers and how many elements to transfer
  // Figure out the sub-array each subExecutor works on for this Transfer
  // - Partition N as evenly as possible, but try to keep subarray sizes as multiples of BLOCK_BYTES bytes,
  //   except the very last one, for alignment reasons
  size_t const N              = this->numBytesActual / sizeof(float);
  int    const initOffset     = ev.byteOffset / sizeof(float);
  int    const targetMultiple = ev.blockBytes / sizeof(float);

  // In some cases, there may not be enough data for all subExectors
  int const maxSubExecToUse = std::min((size_t)(N + targetMultiple - 1) / targetMultiple, (size_t)this->numSubExecs);
  this->subExecParam.clear();
  this->subExecParam.resize(this->numSubExecs);

  size_t assigned = 0;
  for (int i = 0; i < this->numSubExecs; ++i)
  {
    int    const subExecLeft = std::max(0, maxSubExecToUse - i);
    size_t const leftover    = N - assigned;
    size_t const roundedN    = (leftover + targetMultiple - 1) / targetMultiple;

    SubExecParam& p = this->subExecParam[i];
    p.N             = subExecLeft ? std::min(leftover, ((roundedN / subExecLeft) * targetMultiple)) : 0;
    p.numSrcs       = this->numSrcs;
    p.numDsts       = this->numDsts;
    for (int iSrc = 0; iSrc < this->numSrcs; ++iSrc)
      p.src[iSrc] = this->srcMem[iSrc] + assigned + initOffset;
    for (int iDst = 0; iDst < this->numDsts; ++iDst)
      p.dst[iDst] = this->dstMem[iDst] + assigned + initOffset;

    p.preferredXccId = -1;
    if (ev.useXccFilter)
    {
      if (this->exeType == EXE_GPU_GFX && this->numDsts == 1 && IsGpuType(this->dstType[0]))
      {
        p.preferredXccId = ev.prefXccTable[this->exeIndex][this->dstIndex[0]];
      }
    }

    if (ev.enableDebug)
    {
      printf("Transfer %02d SE:%02d: %10lu floats: %10lu to %10lu\n",
             this->transferIndex, i, p.N, assigned, assigned + p.N);
    }

    p.startCycle = 0;
    p.stopCycle  = 0;
    assigned += p.N;
  }

  this->transferTime = 0.0;
  this->perIterationTime.clear();
}

void Transfer::PrepareReference(EnvVars const& ev, std::vector<float>& buffer, int bufferIdx)
{
  size_t N = buffer.size();
  if (bufferIdx >= 0)
  {
    size_t patternLen = ev.fillPattern.size();
    if (patternLen > 0)
    {
      for (size_t i = 0; i < N; ++i)
        buffer[i] = ev.fillPattern[i % patternLen];
    }
    else
    {
      for (size_t i = 0; i < N; ++i)
        buffer[i] = PrepSrcValue(bufferIdx, i);
    }
  }
  else // Destination buffer
  {
    if (this->numSrcs == 0)
    {
      // Note: 0x75757575 = 13323083.0
      memset(buffer.data(), MEMSET_CHAR, N * sizeof(float));
    }
    else
    {
      PrepareReference(ev, buffer, 0);

      if (this->numSrcs > 1)
      {
        std::vector<float> temp(N);
        for (int srcIdx = 1; srcIdx < this->numSrcs; ++srcIdx)
        {
          PrepareReference(ev, temp, srcIdx);
          for (int i = 0; i < N; ++i)
          {
            buffer[i] += temp[i];
          }
        }
      }
    }
  }
}

bool Transfer::PrepareSrc(EnvVars const& ev)
{
  if (this->numSrcs == 0) return true;
  size_t const N = this->numBytesActual / sizeof(float);
  int const initOffset = ev.byteOffset / sizeof(float);

  std::vector<float> reference(N);
  for (int srcIdx = 0; srcIdx < this->numSrcs; ++srcIdx)
  {
    float* srcPtr = this->srcMem[srcIdx] + initOffset;
    PrepareReference(ev, reference, srcIdx);

    // Initialize source memory array with reference pattern
    if (IsGpuType(this->srcType[srcIdx]))
    {
      int const deviceIdx = RemappedIndex(this->srcIndex[srcIdx], false);
      HIP_CALL(hipSetDevice(deviceIdx));
      if (ev.usePrepSrcKernel)
        PrepSrcDataKernel<<<32, ev.blockSize>>>(srcPtr, N, srcIdx);
      else
        HIP_CALL(hipMemcpy(srcPtr, reference.data(), this->numBytesActual, hipMemcpyDefault));
      HIP_CALL(hipDeviceSynchronize());
    }
    else if (IsCpuType(this->srcType[srcIdx]))
    {
      memcpy(srcPtr, reference.data(), this->numBytesActual);
    }

    // Perform check just to make sure that data has been copied properly
    float* srcCheckPtr = srcPtr;
    std::vector<float> srcCopy(N);
    if (IsGpuType(this->srcType[srcIdx]))
    {
      if (!ev.validateDirect)
      {
        HIP_CALL(hipMemcpy(srcCopy.data(), srcPtr, this->numBytesActual, hipMemcpyDefault));
        HIP_CALL(hipDeviceSynchronize());
        srcCheckPtr = srcCopy.data();
      }
    }

    for (size_t i = 0; i < N; ++i)
    {
      if (reference[i] != srcCheckPtr[i])
      {
        printf("\n[ERROR] Unexpected mismatch at index %lu of source array %d:\n", i, srcIdx);
#if !defined(__NVCC__)
        float const val = this->srcMem[srcIdx][initOffset + i];
        printf("[ERROR] SRC %02d   value: %10.5f [%08X] Direct: %10.5f [%08X]\n",
               srcIdx, srcCheckPtr[i], *(unsigned int*)&srcCheckPtr[i], val, *(unsigned int*)&val);
#else
        printf("[ERROR] SRC %02d   value: %10.5f [%08X]\n", srcIdx, srcCheckPtr[i], *(unsigned int*)&srcCheckPtr[i]);
#endif
        printf("[ERROR] EXPECTED value: %10.5f [%08X]\n", reference[i], *(unsigned int*)&reference[i]);
        printf("[ERROR] Failed Transfer details: #%d: %s -> [%c%d:%d] -> %s\n",
               this->transferIndex,
               this->SrcToStr().c_str(),
               ExeTypeStr[this->exeType], this->exeIndex,
               this->numSubExecs,
               this->DstToStr().c_str());
        if (!ev.continueOnError)
          exit(1);
        return false;
      }
    }
  }
  return true;
}

void Transfer::ValidateDst(EnvVars const& ev)
{
  if (this->numDsts == 0) return;
  size_t const N = this->numBytesActual / sizeof(float);
  int const initOffset = ev.byteOffset / sizeof(float);

  std::vector<float> reference(N);
  PrepareReference(ev, reference, -1);

  std::vector<float> hostBuffer(N);
  for (int dstIdx = 0; dstIdx < this->numDsts; ++dstIdx)
  {
    float* output;
    if (IsCpuType(this->dstType[dstIdx]) || ev.validateDirect)
    {
      output = this->dstMem[dstIdx] + initOffset;
    }
    else
    {
      int const deviceIdx = RemappedIndex(this->dstIndex[dstIdx], false);
      HIP_CALL(hipSetDevice(deviceIdx));
      HIP_CALL(hipMemcpy(hostBuffer.data(), this->dstMem[dstIdx] + initOffset, this->numBytesActual, hipMemcpyDefault));
      HIP_CALL(hipDeviceSynchronize());
      output = hostBuffer.data();
    }

    for (size_t i = 0; i < N; ++i)
    {
      if (reference[i] != output[i])
      {
        printf("\n[ERROR] Unexpected mismatch at index %lu of destination array %d:\n", i, dstIdx);
        for (int srcIdx = 0; srcIdx < this->numSrcs; ++srcIdx)
        {
          float srcVal;
          HIP_CALL(hipMemcpy(&srcVal, this->srcMem[srcIdx] + initOffset + i, sizeof(float), hipMemcpyDefault));
#if !defined(__NVCC__)
          float val = this->srcMem[srcIdx][initOffset + i];
          printf("[ERROR] SRC %02dD  value: %10.5f [%08X] Direct: %10.5f [%08X]\n",
                 srcIdx, srcVal, *(unsigned int*)&srcVal, val, *(unsigned int*)&val);
#else
          printf("[ERROR] SRC %02d   value: %10.5f [%08X]\n", srcIdx, srcVal, *(unsigned int*)&srcVal);
#endif
        }
        printf("[ERROR] EXPECTED value: %10.5f [%08X]\n", reference[i], *(unsigned int*)&reference[i]);
#if !defined(__NVCC__)
        float dstVal = this->dstMem[dstIdx][initOffset + i];
        printf("[ERROR] DST %02d   value: %10.5f [%08X] Direct: %10.5f [%08X]\n",
               dstIdx, output[i], *(unsigned int*)&output[i], dstVal, *(unsigned int*)&dstVal);
#else
        printf("[ERROR] DST %02d   value: %10.5f [%08X]\n", dstIdx, output[i], *(unsigned int*)&output[i]);
#endif
        printf("[ERROR] Failed Transfer details: #%d: %s -> [%c%d:%d] -> %s\n",
               this->transferIndex,
               this->SrcToStr().c_str(),
               ExeTypeStr[this->exeType], this->exeIndex,
               this->numSubExecs,
               this->DstToStr().c_str());
        if (!ev.continueOnError)
          exit(1);
        else
          break;
      }
    }
  }
}

std::string Transfer::SrcToStr() const
{
  if (numSrcs == 0) return "N";
  std::stringstream ss;
  for (int i = 0; i < numSrcs; ++i)
    ss << MemTypeStr[srcType[i]] << srcIndex[i];
  return ss.str();
}

std::string Transfer::DstToStr() const
{
  if (numDsts == 0) return "N";
  std::stringstream ss;
  for (int i = 0; i < numDsts; ++i)
    ss << MemTypeStr[dstType[i]] << dstIndex[i];
  return ss.str();
}

void RunSweepPreset(EnvVars const& ev, size_t const numBytesPerTransfer, int const numGpuSubExecs, int const numCpuSubExecs, bool const isRandom)
{
  ev.DisplaySweepEnvVars();

  // Compute how many possible Transfers are permitted (unique SRC/EXE/DST triplets)
  std::vector<std::pair<ExeType, int>> exeList;
  for (auto exe : ev.sweepExe)
  {
    ExeType const exeType = CharToExeType(exe);
    if (IsGpuType(exeType))
    {
      for (int exeIndex = 0; exeIndex < ev.numGpuDevices; ++exeIndex)
        exeList.push_back(std::make_pair(exeType, exeIndex));
    }
    else if (IsCpuType(exeType))
    {
      for (int exeIndex = 0; exeIndex < ev.numCpuDevices; ++exeIndex)
      {
        // Skip NUMA nodes that have no CPUs (e.g. CXL)
        if (ev.numCpusPerNuma[exeIndex] == 0) continue;
        exeList.push_back(std::make_pair(exeType, exeIndex));
      }
    }
  }
  int numExes = exeList.size();

  std::vector<std::pair<MemType, int>> srcList;
  for (auto src : ev.sweepSrc)
  {
    MemType const srcType = CharToMemType(src);
    int const numDevices = IsGpuType(srcType) ? ev.numGpuDevices : ev.numCpuDevices;

    for (int srcIndex = 0; srcIndex < numDevices; ++srcIndex)
      srcList.push_back(std::make_pair(srcType, srcIndex));
  }
  int numSrcs = srcList.size();


  std::vector<std::pair<MemType, int>> dstList;
  for (auto dst : ev.sweepDst)
  {
    MemType const dstType = CharToMemType(dst);
    int const numDevices = IsGpuType(dstType) ? ev.numGpuDevices : ev.numCpuDevices;

    for (int dstIndex = 0; dstIndex < numDevices; ++dstIndex)
      dstList.push_back(std::make_pair(dstType, dstIndex));
  }
  int numDsts = dstList.size();

  // Build array of possibilities, respecting any additional restrictions (e.g. XGMI hop count)
  struct TransferInfo
  {
    MemType srcType; int srcIndex;
    ExeType exeType; int exeIndex;
    MemType dstType; int dstIndex;
  };

  // If either XGMI minimum is non-zero, or XGMI maximum is specified and non-zero then both links must be XGMI
  bool const useXgmiOnly = (ev.sweepXgmiMin > 0 || ev.sweepXgmiMax > 0);

  std::vector<TransferInfo> possibleTransfers;
  TransferInfo tinfo;
  for (int i = 0; i < numExes; ++i)
  {
    // Skip CPU executors if XGMI link must be used
    if (useXgmiOnly && !IsGpuType(exeList[i].first)) continue;
    tinfo.exeType  = exeList[i].first;
    tinfo.exeIndex = exeList[i].second;

    bool isXgmiSrc  = false;
    int  numHopsSrc = 0;
    for (int j = 0; j < numSrcs; ++j)
    {
      if (IsGpuType(exeList[i].first) && IsGpuType(srcList[j].first))
      {
        if (exeList[i].second != srcList[j].second)
        {
#if defined(__NVCC__)
          isXgmiSrc = false;
#else
          uint32_t exeToSrcLinkType, exeToSrcHopCount;
          HIP_CALL(hipExtGetLinkTypeAndHopCount(RemappedIndex(exeList[i].second, false),
                                                RemappedIndex(srcList[j].second, false),
                                                &exeToSrcLinkType,
                                                &exeToSrcHopCount));
          isXgmiSrc = (exeToSrcLinkType == HSA_AMD_LINK_INFO_TYPE_XGMI);
          if (isXgmiSrc) numHopsSrc = exeToSrcHopCount;
#endif
        }
        else
        {
          isXgmiSrc = true;
          numHopsSrc = 0;
        }

        // Skip this SRC if it is not XGMI but only XGMI links may be used
        if (useXgmiOnly && !isXgmiSrc) continue;

        // Skip this SRC if XGMI distance is already past limit
        if (ev.sweepXgmiMax >= 0 && isXgmiSrc && numHopsSrc > ev.sweepXgmiMax) continue;
      }
      else if (useXgmiOnly) continue;

      tinfo.srcType  = srcList[j].first;
      tinfo.srcIndex = srcList[j].second;

      bool isXgmiDst = false;
      int  numHopsDst = 0;
      for (int k = 0; k < numDsts; ++k)
      {
        if (IsGpuType(exeList[i].first) && IsGpuType(dstList[k].first))
        {
          if (exeList[i].second != dstList[k].second)
          {
#if defined(__NVCC__)
            isXgmiSrc = false;
#else
            uint32_t exeToDstLinkType, exeToDstHopCount;
            HIP_CALL(hipExtGetLinkTypeAndHopCount(RemappedIndex(exeList[i].second, false),
                                                  RemappedIndex(dstList[k].second, false),
                                                  &exeToDstLinkType,
                                                  &exeToDstHopCount));
            isXgmiDst = (exeToDstLinkType == HSA_AMD_LINK_INFO_TYPE_XGMI);
            if (isXgmiDst) numHopsDst = exeToDstHopCount;
#endif
          }
          else
          {
            isXgmiDst = true;
            numHopsDst = 0;
          }
        }

        // Skip this DST if it is not XGMI but only XGMI links may be used
        if (useXgmiOnly && !isXgmiDst) continue;

        // Skip this DST if total XGMI distance (SRC + DST) is less than min limit
        if (ev.sweepXgmiMin > 0 && (numHopsSrc + numHopsDst < ev.sweepXgmiMin)) continue;

        // Skip this DST if total XGMI distance (SRC + DST) is greater than max limit
        if (ev.sweepXgmiMax >= 0 && (numHopsSrc + numHopsDst) > ev.sweepXgmiMax) continue;

#if defined(__NVCC__)
        // Skip CPU executors on GPU memory on NVIDIA platform
        if (IsCpuType(exeList[i].first) && (IsGpuType(dstList[j].first) || IsGpuType(dstList[k].first)))
          continue;
#endif

        tinfo.dstType  = dstList[k].first;
        tinfo.dstIndex = dstList[k].second;

        possibleTransfers.push_back(tinfo);
      }
    }
  }

  int const numPossible = (int)possibleTransfers.size();
  int maxParallelTransfers = (ev.sweepMax == 0 ? numPossible : ev.sweepMax);

  if (ev.sweepMin > numPossible)
  {
    printf("No valid test configurations exist\n");
    return;
  }

  if (ev.outputToCsv)
  {
    printf("\nTest#,Transfer#,NumBytes,Src,Exe,Dst,CUs,BW(GB/s),Time(ms),"
           "ExeToSrcLinkType,ExeToDstLinkType,SrcAddr,DstAddr\n");
  }

  int numTestsRun = 0;
  int M = ev.sweepMin;
  std::uniform_int_distribution<int> randSize(1, numBytesPerTransfer / sizeof(float));
  std::uniform_int_distribution<int> distribution(ev.sweepMin, maxParallelTransfers);

  // Log sweep to configuration file
  FILE *fp = fopen("lastSweep.cfg", "w");
  if (!fp)
  {
    printf("[ERROR] Unable to open lastSweep.cfg.  Check permissions\n");
    exit(1);
  }

  // Create bitmask of numPossible triplets, of which M will be chosen
  std::string bitmask(M, 1);  bitmask.resize(numPossible, 0);
  auto cpuStart = std::chrono::high_resolution_clock::now();
  while (1)
  {
    if (isRandom)
    {
      // Pick random number of simultaneous transfers to execute
      // NOTE: This currently skews distribution due to some #s having more possibilities than others
      M = distribution(*ev.generator);

      // Generate a random bitmask
      for (int i = 0; i < numPossible; i++)
        bitmask[i] = (i < M) ? 1 : 0;
      std::shuffle(bitmask.begin(), bitmask.end(), *ev.generator);
    }

    // Convert bitmask to list of Transfers
    std::vector<Transfer> transfers;
    for (int value = 0; value < numPossible; ++value)
    {
      if (bitmask[value])
      {
        // Convert integer value to (SRC->EXE->DST) triplet
        Transfer transfer;
        transfer.numSrcs        = 1;
        transfer.numDsts        = 1;
        transfer.srcType        = {possibleTransfers[value].srcType};
        transfer.srcIndex       = {possibleTransfers[value].srcIndex};
        transfer.exeType        = possibleTransfers[value].exeType;
        transfer.exeIndex       = possibleTransfers[value].exeIndex;
        transfer.dstType        = {possibleTransfers[value].dstType};
        transfer.dstIndex       = {possibleTransfers[value].dstIndex};
        transfer.numSubExecs    = IsGpuType(transfer.exeType) ? numGpuSubExecs : numCpuSubExecs;
        transfer.numBytes       = ev.sweepRandBytes ? randSize(*ev.generator) * sizeof(float) : 0;
        transfers.push_back(transfer);
      }
    }

    LogTransfers(fp, ++numTestsRun, transfers);
    ExecuteTransfers(ev, numTestsRun, numBytesPerTransfer / sizeof(float), transfers);

    // Check for test limit
    if (numTestsRun == ev.sweepTestLimit)
    {
      printf("Test limit reached\n");
      break;
    }

    // Check for time limit
    auto cpuDelta = std::chrono::high_resolution_clock::now() - cpuStart;
    double totalCpuTime = std::chrono::duration_cast<std::chrono::duration<double>>(cpuDelta).count();
    if (ev.sweepTimeLimit && totalCpuTime > ev.sweepTimeLimit)
    {
      printf("Time limit exceeded\n");
      break;
    }

    // Increment bitmask if not random sweep
    if (!isRandom && !std::prev_permutation(bitmask.begin(), bitmask.end()))
    {
      M++;
      // Check for completion
      if (M > maxParallelTransfers)
      {
        printf("Sweep complete\n");
        break;
      }
      for (int i = 0; i < numPossible; i++)
        bitmask[i] = (i < M) ? 1 : 0;
    }
  }
  fclose(fp);
}

void LogTransfers(FILE *fp, int const testNum, std::vector<Transfer> const& transfers)
{
  fprintf(fp, "# Test %d\n", testNum);
  fprintf(fp, "%d", -1 * (int)transfers.size());
  for (auto const& transfer : transfers)
  {
    fprintf(fp, " (%c%d->%c%d->%c%d %d %lu)",
            MemTypeStr[transfer.srcType[0]], transfer.srcIndex[0],
            ExeTypeStr[transfer.exeType],    transfer.exeIndex,
            MemTypeStr[transfer.dstType[0]], transfer.dstIndex[0],
            transfer.numSubExecs,
            transfer.numBytes);
  }
  fprintf(fp, "\n");
  fflush(fp);
}

std::string PtrVectorToStr(std::vector<float*> const& strVector, int const initOffset)
{
  std::stringstream ss;
  for (int i = 0; i < strVector.size(); ++i)
  {
    if (i) ss << " ";
    ss << (strVector[i] + initOffset);
  }
  return ss.str();
}
