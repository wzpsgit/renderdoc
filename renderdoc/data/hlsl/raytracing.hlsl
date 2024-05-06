/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "hlsl_cbuffers.h"

RWStructuredBuffer<InstanceDesc> instanceDescs : register(u0);
StructuredBuffer<BlasAddressPair> oldNewAddressesPair : register(t0);

bool InRange(BlasAddressRange addressRange, GPUAddress address)
{
  if(lessEqual(addressRange.start, address) && lessThan(address, addressRange.end))
  {
    return true;
  }

  return false;
}

// Each SV_GroupId corresponds to each of the BLAS (instance) in TLAS
[numthreads(1, 1, 1)] void RENDERDOC_PatchAccStructAddressCS(uint3 dispatchGroup
                                                             : SV_GroupId) {
  GPUAddress instanceBlasAddress = instanceDescs[dispatchGroup.x].blasAddress;

  for(uint i = 0; i < addressCount; i++)
  {
    if(InRange(oldNewAddressesPair[i].oldAddress, instanceBlasAddress))
    {
      GPUAddress offset = sub(instanceBlasAddress, oldNewAddressesPair[i].oldAddress.start);
      instanceDescs[dispatchGroup.x].blasAddress =
          add(oldNewAddressesPair[i].newAddress.start, offset);
      return;
    }
  }

  // This  might cause device hang but at least we won't access incorrect addresses
  instanceDescs[dispatchGroup.x].blasAddress = 0;
}

struct StateObjectLookup
{
  uint2 id;    // ResourceId
  uint offset;
};

StructuredBuffer<StateObjectLookup> stateObjects : register(t0);

struct RecordData
{
  uint4 identifier[2];    // 32-byte real identifier
  uint rootSigIndex;      // only lower 16-bits are valid
};

StructuredBuffer<RecordData> records : register(t1);

struct RootSig
{
  uint numHandles;
  uint handleOffsets[MAX_LOCALSIG_HANDLES];
};

StructuredBuffer<RootSig> rootsigs : register(t2);

struct WrappedRecord
{
  uint2 id;    // ResourceId
  uint index;
};

RWByteAddressBuffer bufferToPatch : register(u0);

struct DescriptorHeapData
{
  GPUAddress wrapped_base;
  GPUAddress wrapped_end;

  GPUAddress unwrapped_base;

  uint unwrapped_stride;
};

void PatchTable(uint byteOffset)
{
  // load our wrapped record from the start of the table
  WrappedRecord wrappedRecord;
  wrappedRecord.id = bufferToPatch.Load2(byteOffset);
  wrappedRecord.index = bufferToPatch.Load(byteOffset + 8);

  // find the state object it came from
  int i = 0;
  StateObjectLookup objectLookup;
  do
  {
    objectLookup = stateObjects[i];

    if(objectLookup.id.x == wrappedRecord.id.x && objectLookup.id.y == wrappedRecord.id.y)
      break;

    // terminate when the lookup is empty, we're out of state objects
  } while(objectLookup.id.x != 0 || objectLookup.id.y != 0);

  // if didn't find a match, set a NULL shader identifier. This will fail if it's raygen but others
  // will in theory not crash.
  if(objectLookup.id.x == 0 && objectLookup.id.y == 0)
  {
    bufferToPatch.Store4(byteOffset, uint4(0, 0, 0, 0));
    bufferToPatch.Store4(byteOffset + 16, uint4(0, 0, 0, 0));
    return;
  }

  // the exports from this state object are contiguous starting from the given index, look up this
  // identifier's export
  RecordData recordData = records[objectLookup.offset + wrappedRecord.index];

  // store the unwrapped shader identifier
  bufferToPatch.Store4(byteOffset, recordData.identifier[0]);
  bufferToPatch.Store4(byteOffset + 16, recordData.identifier[1]);

  if(recordData.rootSigIndex & 0xffff != 0xffff)
  {
    RootSig sig = rootsigs[recordData.rootSigIndex];

    DescriptorHeapData heaps[2];

    heaps[0].wrapped_base = wrapped_sampHeapBase;
    heaps[1].wrapped_base = wrapped_srvHeapBase;

    heaps[0].wrapped_end = add(wrapped_sampHeapBase, GPUAddress(wrapped_sampHeapSize, 0));
    heaps[1].wrapped_end = add(wrapped_srvHeapBase, GPUAddress(wrapped_srvHeapSize, 0));

    heaps[0].unwrapped_stride = unwrapped_heapStrides & 0xffff;
    heaps[1].unwrapped_stride = unwrapped_heapStrides >> 16;

    heaps[0].unwrapped_base = unwrapped_sampHeapBase;
    heaps[1].unwrapped_base = unwrapped_srvHeapBase;

    for(uint i = 0; i < sig.numHandles; i++)
    {
      GPUAddress wrappedHandlePtr = bufferToPatch.Load2(sig.handleOffsets[i]);

      bool patched = false;
      for(int h = 0; h < 2; h++)
      {
        if(lessEqual(heaps[h].wrapped_base, wrappedHandlePtr) &&
           lessThan(wrappedHandlePtr, heaps[h].wrapped_end))
        {
          // assume the byte offsets will all fit into the LSB 32-bits
          uint index = sub(wrappedHandlePtr, wrapped_sampHeapBase).x / WRAPPED_DESCRIPTOR_STRIDE;
          GPUAddress handleOffset = GPUAddress(index * heaps[h].unwrapped_stride, 0);
          bufferToPatch.Store2(sig.handleOffsets[i], add(heaps[h].unwrapped_base, handleOffset));
          patched = true;
        }
      }

      if(!patched)
      {
        // won't work but is our best effort
        bufferToPatch.Store2(sig.handleOffsets[i], GPUAddress(0, 0));
      }
    }
  }
}

// Each SV_GroupId corresponds to one shader record to patch
[numthreads(1, 1, 1)] void RENDERDOC_PatchRayDispatchCS(uint3 dispatchGroup
                                                        : SV_GroupId) {
  uint group = dispatchGroup.x;

  if(group == 0)
  {
    PatchTable(0);
    return;
  }

  group--;

  if(group < raydispatch_misscount)
  {
    PatchTable(raydispatch_missoffs + raydispatch_missstride * group);
    return;
  }

  group -= raydispatch_misscount;

  if(group < raydispatch_hitcount)
  {
    PatchTable(raydispatch_hitoffs + raydispatch_hitstride * group);
    return;
  }

  group -= raydispatch_hitcount;

  if(group < raydispatch_callcount)
  {
    PatchTable(raydispatch_calloffs + raydispatch_callstride * group);
    return;
  }
}
