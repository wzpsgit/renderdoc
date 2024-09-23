//==============================================================================
// Copyright (c) 2010-2021 Advanced Micro Devices, Inc. All rights reserved.
/// \author AMD Developer Tools Team
/// \file
/// \brief  Deprecated header; use gpu_performance_api/gpu_perf_api_function_types.h
//==============================================================================

#ifndef GPU_PERF_API_FUNCTION_TYPES_H_
#define GPU_PERF_API_FUNCTION_TYPES_H_

#pragma message("Warning: You are including a deprecated header. Please use gpu_performance_api/gpu_perf_api_function_types.h")

// New header
#include "gpu_performance_api/gpu_perf_api_function_types.h"

// Deprecated header
#include "gpu_perf_api_types.h"

// GPA API Version
typedef GPA_Status (*GPA_GetVersionPtrType)(gpa_uint32*, gpa_uint32*, gpa_uint32*, gpa_uint32*);  ///< Typedef for a function pointer for GPA_GetVersion

// GPA API Table
typedef GPA_Status (*GPA_GetFuncTablePtrType)(void*);  ///< Typedef for a function pointer for GPA_GetFuncTable

// Logging
typedef void (*GPA_LoggingCallbackPtrType)(GPA_Logging_Type, const char*);  ///< Typedef for a function pointer for a logging callback function

typedef GPA_Status (*GPA_RegisterLoggingCallbackPtrType)(GPA_Logging_Type,
                                                         GPA_LoggingCallbackPtrType);  ///< Typedef for a function pointer for GPA_RegisterLoggingCallback

// Init / Destroy GPA
typedef GPA_Status (*GPA_InitializePtrType)(GPA_InitializeFlags);  ///< Typedef for a function pointer for GPA_Initialize
typedef GPA_Status (*GPA_DestroyPtrType)();                        ///< Typedef for a function pointer for GPA_Destroy

// Context Startup / Finish
typedef GPA_Status (*GPA_OpenContextPtrType)(void*, GPA_OpenContextFlags, GPA_ContextId*);  ///< Typedef for a function pointer for GPA_OpenContext
typedef GPA_Status (*GPA_CloseContextPtrType)(GPA_ContextId);                               ///< Typedef for a function pointer for GPA_CloseContext

// Context Interrogation
typedef GPA_Status (*GPA_GetSupportedSampleTypesPtrType)(GPA_ContextId,
                                                         GPA_ContextSampleTypeFlags*);  ///< Typedef for a function pointer for GPA_GetSupportedSampleTypes
typedef GPA_Status (*GPA_GetDeviceAndRevisionIdPtrType)(GPA_ContextId,
                                                        gpa_uint32*,
                                                        gpa_uint32*);         ///< Typedef for a function pointer for GPA_GetDeviceAndRevisionId
typedef GPA_Status (*GPA_GetDeviceNamePtrType)(GPA_ContextId, const char**);  ///< Typedef for a function pointer for GPA_GetDeviceName

#ifdef GPA_PIX_BUILD
typedef GPA_Status (*GPA_UpdateDeviceInformationPtrType)(GPA_ContextId,
                                                         gpa_uint32,
                                                         gpa_uint32,
                                                         gpa_uint32,
                                                         gpa_uint32);  ///< Typedef for a function pointer for GPA_UpdateDeviceInformation
#endif // GPA_PIX_BUILD

#ifndef GPA_PIX_BUILD
// Counter Interrogation
typedef GPA_Status(*GPA_GetNumCountersPtrType)(GPA_ContextId, gpa_uint32*);                ///< Typedef for a function pointer for GPA_GetNumCounters
typedef GPA_Status(*GPA_GetCounterNamePtrType)(GPA_ContextId, gpa_uint32, const char**);   ///< Typedef for a function pointer for GPA_GetCounterName
typedef GPA_Status(*GPA_GetCounterIndexPtrType)(GPA_ContextId, const char*, gpa_uint32*);  ///< Typedef for a function pointer for GPA_GetCounterIndex
typedef GPA_Status(*GPA_GetCounterGroupPtrType)(GPA_ContextId, gpa_uint32, const char**);  ///< Typedef for a function pointer for GPA_GetCounterGroup
typedef GPA_Status(*GPA_GetCounterDescriptionPtrType)(GPA_ContextId,
    gpa_uint32,
    const char**);  ///< Typedef for a function pointer for GPA_GetCounterDescription
typedef GPA_Status(*GPA_GetCounterDataTypePtrType)(GPA_ContextId, gpa_uint32, GPA_Data_Type*);  ///< Typedef for a function pointer for GPA_GetCounterDataType
typedef GPA_Status(*GPA_GetCounterUsageTypePtrType)(GPA_ContextId,
    gpa_uint32,
    GPA_Usage_Type*);                  ///< Typedef for a function pointer for GPA_GetCounterUsageType
typedef GPA_Status(*GPA_GetCounterUuidPtrType)(GPA_ContextId, gpa_uint32, GPA_UUID*);  ///< Typedef for a function pointer for GPA_GetCounterUuid
typedef GPA_Status(*GPA_GetCounterSampleTypePtrType)(GPA_ContextId,
    gpa_uint32,
    GPA_Counter_Sample_Type*);   ///< Typedef for a function pointer for GPA_GetCounterSampleType

#else
// Counter Interrogation
typedef GPA_Status (*GPA_GetNumCountersPtrType)(GPA_SessionId, gpa_uint32*);                ///< Typedef for a function pointer for GPA_GetNumCounters
typedef GPA_Status (*GPA_GetCounterNamePtrType)(GPA_SessionId, gpa_uint32, const char**);   ///< Typedef for a function pointer for GPA_GetCounterName
typedef GPA_Status (*GPA_GetCounterIndexPtrType)(GPA_SessionId, const char*, gpa_uint32*);  ///< Typedef for a function pointer for GPA_GetCounterIndex
typedef GPA_Status (*GPA_GetCounterGroupPtrType)(GPA_SessionId, gpa_uint32, const char**);  ///< Typedef for a function pointer for GPA_GetCounterGroup
typedef GPA_Status (*GPA_GetCounterDescriptionPtrType)(GPA_SessionId,
                                                       gpa_uint32,
                                                       const char**);  ///< Typedef for a function pointer for GPA_GetCounterDescription
typedef GPA_Status (*GPA_GetCounterDataTypePtrType)(GPA_SessionId, gpa_uint32, GPA_Data_Type*);  ///< Typedef for a function pointer for GPA_GetCounterDataType
typedef GPA_Status (*GPA_GetCounterUsageTypePtrType)(GPA_SessionId,
                                                     gpa_uint32,
                                                     GPA_Usage_Type*);                  ///< Typedef for a function pointer for GPA_GetCounterUsageType
typedef GPA_Status (*GPA_GetCounterUuidPtrType)(GPA_SessionId, gpa_uint32, GPA_UUID*);  ///< Typedef for a function pointer for GPA_GetCounterUuid
typedef GPA_Status (*GPA_GetCounterSampleTypePtrType)(GPA_SessionId,
                                                      gpa_uint32,
                                                      GPA_Counter_Sample_Type*);   ///< Typedef for a function pointer for GPA_GetCounterSampleType
#endif // GPA_PIX_BUILD
typedef GPA_Status (*GPA_GetDataTypeAsStrPtrType)(GPA_Data_Type, const char**);    ///< Typedef for a function pointer for GPA_GetDataTypeAsStr
typedef GPA_Status (*GPA_GetUsageTypeAsStrPtrType)(GPA_Usage_Type, const char**);  ///< Typedef for a function pointer for GPA_GetUsageTypeAsStr

// Session handling
typedef GPA_Status (*GPA_CreateSessionPtrType)(GPA_ContextId,
                                               GPA_Session_Sample_Type,
                                               GPA_SessionId*);  ///< Typedef for a function pointer for GPA_CreateSession
typedef GPA_Status (*GPA_DeleteSessionPtrType)(GPA_SessionId);   ///< Typedef for a function pointer for GPA_DeleteSession
typedef GPA_Status (*GPA_BeginSessionPtrType)(GPA_SessionId);    ///< Typedef for a function pointer for GPA_BeginSession
#ifdef GPA_PIX_BUILD
typedef GPA_Status (*GPA_RestartSessionPtrType)(GPA_SessionId);  ///< Typedef for a function pointer for GPA_RestartSession
#endif // GPA_PIX_BUILD
typedef GPA_Status (*GPA_EndSessionPtrType)(GPA_SessionId);      ///< Typedef for a function pointer for GPA_EndSession
#ifdef GPA_PIX_BUILD
typedef GPA_Status (*GPA_AbortSessionPtrType)(GPA_SessionId);    ///< Typedef for a function pointer for GPA_AbortSession
#endif // GPA_PIX_BUILD


#ifdef GPA_PIX_BUILD
// SQTT functions
typedef GPA_Status (*GPA_SqttGetInstructionMaskPtrType)(GPA_SessionId,
                                                        GPA_SqttInstructionFlags*);  ///< Typedef for a function pointer for GPA_GetSqttDoIncludeInstructions
typedef GPA_Status (*GPA_SqttSetInstructionMaskPtrType)(GPA_SessionId,
                                                        GPA_SqttInstructionFlags);  ///< Typedef for a function pointer for GPA_SetSqttDoIncludeInstructions
typedef GPA_Status (*GPA_SqttGetComputeUnitIdPtrType)(GPA_SessionId, gpa_uint32*);  ///< Typedef for a function pointer for GPA_GetSqttComputeUnitId
typedef GPA_Status (*GPA_SqttSetComputeUnitIdPtrType)(GPA_SessionId, gpa_uint32);   ///< Typedef for a function pointer for GPA_SetSqttComputeUnitId

typedef GPA_Status (*GPA_SqttWarmupPtrType)(GPA_SessionId);                          ///< Typedef for a function pointer for GPA_BeginWarmup
typedef GPA_Status (*GPA_SqttBeginPtrType)(GPA_SessionId, void*);                    ///< Typedef for a function pointer for GPA_BeginSqtt
typedef GPA_Status (*GPA_SqttEndPtrType)(GPA_SessionId, void*);                      ///< Typedef for a function pointer for GPA_EndSqtt
typedef GPA_Status (*GPA_SqttGetSampleResultSizePtrType)(GPA_SessionId, size_t*);    ///< Typedef for a function pointer for GPA_SqttGetSampleResultSize
typedef GPA_Status (*GPA_SqttGetSampleResultPtrType)(GPA_SessionId, size_t, void*);  ///< Typedef for a function pointer for GPA_SqttGetSampleResult

// SPM functions
typedef GPA_Status (*GPA_SpmSetSampleIntervalPtrType)(GPA_SessionId, gpa_uint32);   ///< Typedef for a function pointer for GPA_SpmSetSampleInterval
typedef GPA_Status (*GPA_SpmSetDurationPtrType)(GPA_SessionId, gpa_uint32);         ///< Typedef for a function pointer for GPA_SpmSetDuration
typedef GPA_Status (*GPA_SpmBeginPtrType)(GPA_SessionId, void*);                    ///< Typedef for a function pointer for SpmBegin
typedef GPA_Status (*GPA_SpmEndPtrType)(GPA_SessionId, void*);                      ///< Typedef for a function pointer for SpmEnd
typedef GPA_Status (*GPA_SpmGetSampleResultSizePtrType)(GPA_SessionId, size_t*);    ///< Typedef for a function pointer for SpmGetSampleResultSize
typedef GPA_Status (*GPA_SpmGetSampleResultPtrType)(GPA_SessionId, size_t, void*);  ///< Typedef for a function pointer for SpmGetSampleResult
typedef GPA_Status (*GPA_SpmCalculateDerivedCountersPtrType)(GPA_SessionId,
                                                             GpaSpmData*,
                                                             gpa_uint32,
                                                             gpa_uint64*);  ///< Typedef for a function pointer for SpmCalculateDerivedCounters
#endif // GPA_PIX_BUILD

// Counter Scheduling
typedef GPA_Status (*GPA_EnableCounterPtrType)(GPA_SessionId, gpa_uint32);          ///< Typedef for a function pointer for GPA_EnableCounter
typedef GPA_Status (*GPA_DisableCounterPtrType)(GPA_SessionId, gpa_uint32);         ///< Typedef for a function pointer for GPA_DisableCounter
typedef GPA_Status (*GPA_EnableCounterByNamePtrType)(GPA_SessionId, const char*);   ///< Typedef for a function pointer for GPA_EnableCounterByName
typedef GPA_Status (*GPA_DisableCounterByNamePtrType)(GPA_SessionId, const char*);  ///< Typedef for a function pointer for GPA_DisableCounterByName
typedef GPA_Status (*GPA_EnableAllCountersPtrType)(GPA_SessionId);                  ///< Typedef for a function pointer for GPA_EnableAllCounters
typedef GPA_Status (*GPA_DisableAllCountersPtrType)(GPA_SessionId);                 ///< Typedef for a function pointer for GPA_DisableAllCounters

// Query Counter Scheduling
typedef GPA_Status (*GPA_GetPassCountPtrType)(GPA_SessionId, gpa_uint32*);                 ///< Typedef for a function pointer for GPA_GetPassCount
typedef GPA_Status (*GPA_GetNumEnabledCountersPtrType)(GPA_SessionId, gpa_uint32*);        ///< Typedef for a function pointer for GetNumEnabledCounters
typedef GPA_Status (*GPA_GetEnabledIndexPtrType)(GPA_SessionId, gpa_uint32, gpa_uint32*);  ///< Typedef for a function pointer for GPA_GetEnabledIndex
typedef GPA_Status (*GPA_IsCounterEnabledPtrType)(GPA_SessionId, gpa_uint32);              ///< Typedef for a function pointer for GPA_IsCounterEnabled

// Sample Handling
typedef GPA_Status (*GPA_BeginCommandListPtrType)(GPA_SessionId,
                                                  gpa_uint32,
                                                  void*,
                                                  GPA_Command_List_Type,
                                                  GPA_CommandListId*);        ///< Typedef for a function pointer for GPA_BeginCommandList
typedef GPA_Status (*GPA_EndCommandListPtrType)(GPA_CommandListId);           ///< Typedef for a function pointer for GPA_EndCommandList
typedef GPA_Status (*GPA_BeginSamplePtrType)(gpa_uint32, GPA_CommandListId);  ///< Typedef for a function pointer for GPA_BeginSample
typedef GPA_Status (*GPA_EndSamplePtrType)(GPA_CommandListId);                ///< Typedef for a function pointer for GPA_EndSample
typedef GPA_Status (*GPA_ContinueSampleOnCommandListPtrType)(gpa_uint32,
                                                             GPA_CommandListId);  ///< Typedef for a function pointer for GPA_ContinueSampleOnCommandList
typedef GPA_Status (*GPA_CopySecondarySamplesPtrType)(GPA_CommandListId,
                                                      GPA_CommandListId,
                                                      gpa_uint32,
                                                      gpa_uint32*);                    ///< Typedef for a function pointer for GPA_CopySecondarySamples
typedef GPA_Status (*GPA_GetSampleCountPtrType)(GPA_SessionId, gpa_uint32*);           ///< Typedef for a function pointer for GPA_GetSampleCount
typedef GPA_Status (*GPA_GetSampleIdPtrType)(GPA_SessionId, gpa_uint32, gpa_uint32*);  ///< Typedef for a function pointer for GPA_GetSampleId

// Query Results
typedef GPA_Status (*GPA_IsSessionCompletePtrType)(GPA_SessionId);                           ///< Typedef for a function pointer for GPA_IsSessionComplete
typedef GPA_Status (*GPA_IsPassCompletePtrType)(GPA_SessionId, gpa_uint32);                  ///< Typedef for a function pointer for GPA_IsPassComplete
typedef GPA_Status (*GPA_GetSampleResultSizePtrType)(GPA_SessionId, gpa_uint32, size_t*);    ///< Typedef for a function pointer for GPA_GetSampleResultSize
typedef GPA_Status (*GPA_GetSampleResultPtrType)(GPA_SessionId, gpa_uint32, size_t, void*);  ///< Typedef for a function pointer for GPA_GetSampleResult

// Status / Error Query
typedef const char* (*GPA_GetStatusAsStrPtrType)(GPA_Status);  ///< Typedef for a function pointer for GPA_GetStatusAsStr

#endif  // GPU_PERF_API_FUNCTION_TYPES_H_
