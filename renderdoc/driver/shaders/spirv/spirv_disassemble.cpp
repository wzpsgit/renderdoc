/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2015 Baldur Karlsson
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

#include "common/common.h"
#include "maths/formatpacking.h"

#include "serialise/serialiser.h"

#include "spirv_common.h"

#include "api/replay/renderdoc_replay.h"

#include <utility>
#include <algorithm>
using std::pair;
using std::make_pair;

#undef min
#undef max

#include "3rdparty/glslang/SPIRV/spirv.hpp"
#include "3rdparty/glslang/SPIRV/GLSL.std.450.h"
#include "3rdparty/glslang/glslang/Public/ShaderLang.h"

// I'm not sure yet if this makes things clearer or worse. On the one hand
// it is explicit about stores/loads through pointers, but on the other it
// produces a lot of noise.
#define LOAD_STORE_CONSTRUCTORS 0

// possibly for consistency all constants should construct themselves, but
// for scalars it's potentially simpler just to drop it.
#define SCALAR_CONSTRUCTORS 0

// don't inline expressions of this complexity or higher
#define NO_INLINE_COMPLEXITY 3

// declare function variables at the top of the scope, rather than at the
// first use of that variable
#define C_VARIABLE_DECLARATIONS 0

namespace spv { Op OpUnknown = (Op)~0U; }

const char *GLSL_STD_450_names[GLSLstd450Count] = {0};

// list of known generators, just for kicks
struct { uint32_t magic; const char *name; } KnownGenerators[] = {
	{ 0x051a00bb, "glslang" },
};

template<typename EnumType>
static string OptionalFlagString(EnumType e)
{
	return (int)e ? " [" + ToStr::Get(e) + "]" : "";
}

static string DefaultIDName(uint32_t ID)
{
	return StringFormat::Fmt("{%u}", ID);
}

template<typename T>
static bool erase_item(std::vector<T> &vec, const T& elem)
{
	auto it = std::find(vec.begin(), vec.end(), elem);
	if(it != vec.end())
	{
		vec.erase(it);
		return true;
	}

	return false;
}

struct SPVInstruction;

struct SPVDecoration
{
	SPVDecoration() : decoration(spv::DecorationRelaxedPrecision), val(0) {}

	spv::Decoration decoration;

	uint32_t val;

	string Str() const
	{
		switch(decoration)
		{
			case spv::DecorationRowMajor:
			case spv::DecorationColMajor:
			case spv::DecorationSmooth:
			case spv::DecorationNoPerspective:
			case spv::DecorationFlat:
			case spv::DecorationCentroid:
			case spv::DecorationGLSLShared:
			case spv::DecorationBlock:
				return ToStr::Get(decoration);
			case spv::DecorationArrayStride: // might hide these, it adds no value
				return StringFormat::Fmt("ArrayStride=%u", val);
			case spv::DecorationMatrixStride: // might hide these, it adds no value
				return StringFormat::Fmt("MatrixStride=%u", val);
			case spv::DecorationLocation:
				return StringFormat::Fmt("Location=%u", val);
			case spv::DecorationBinding:
				return StringFormat::Fmt("Bind=%u", val);
			case spv::DecorationDescriptorSet:
				return StringFormat::Fmt("DescSet=%u", val);
			case spv::DecorationBuiltIn:
				return StringFormat::Fmt("Builtin %s", ToStr::Get((spv::BuiltIn)val).c_str());
			case spv::DecorationSpecId:
				return StringFormat::Fmt("Specialize[%u]", ToStr::Get(decoration).c_str(), val);
			default:
				break;
		}

		return StringFormat::Fmt("%s=%u", ToStr::Get(decoration).c_str(), val);
	}
};

struct SPVExtInstSet
{
	SPVExtInstSet() : instructions(NULL) {}

	string setname;
	const char **instructions;
};

struct SPVExecutionMode
{
	SPVExecutionMode() : mode(spv::ExecutionModeInvocations), x(0), y(0), z(0) {}

	spv::ExecutionMode mode;
	uint32_t x, y, z; // optional params
};

struct SPVEntryPoint
{
	SPVEntryPoint() : func(0), model(spv::ExecutionModelVertex) {}

	// entry point will come before declaring instruction,
	// so we reference the function by ID
	uint32_t func;
	spv::ExecutionModel model;
	string name;
	vector<SPVExecutionMode> modes;
};

struct SPVTypeData
{
	SPVTypeData() :
		baseType(NULL), storage(spv::StorageClassUniformConstant),
			texdim(spv::Dim2D), sampled(2), arrayed(false), depth(false), multisampled(false), imgformat(spv::ImageFormatUnknown),
		bitCount(32), vectorSize(1), matrixSize(1), arraySize(1) {}

	enum
	{
		eVoid,
		eBool,
		eFloat,
		eSInt,
		eUInt,
		eBasicCount,

		eVector,
		eMatrix,
		eArray,
		ePointer,
		eCompositeCount,

		eFunction,

		eStruct,
		eImage,
		eSampler,
		eSampledImage,

		eTypeCount,
	} type;

	SPVTypeData *baseType;

	string name;

	bool IsBasicInt() const
	{
		return type == eUInt || type == eSInt;
	}

	bool IsScalar() const
	{
		return type < eBasicCount && type != eVoid;
	}

	string DeclareVariable(const vector<SPVDecoration> &decorations, const string &varName)
	{
		string ret = "";

		const SPVDecoration *builtin = NULL;

		for(size_t d=0; d < decorations.size(); d++)
		{
			if(decorations[d].decoration == spv::DecorationBuiltIn)
			{
				builtin = &decorations[d];
				continue;
			}
			ret += decorations[d].Str() + " ";
		}

		if(type == ePointer && baseType->type == eArray)
			ret += StringFormat::Fmt("%s* %s[%u]", baseType->baseType->GetName().c_str(), varName.c_str(), baseType->arraySize);
		else if(type == eArray)
			ret += StringFormat::Fmt("%s %s[%u]", baseType->GetName().c_str(), varName.c_str(), arraySize);
		else
			ret += StringFormat::Fmt("%s %s", GetName().c_str(), varName.c_str());

		if(builtin)
			ret += " = " + ToStr::Get((spv::BuiltIn)builtin->val);

		return ret;
	}

	const string &GetName()
	{
		if(name.empty())
		{
			if(type == eVoid)
			{
				name = "void";
			}
			else if(type == eBool)
			{
				name = "bool";
			}
			else if(type == eFloat)
			{
				RDCASSERT(bitCount == 64 || bitCount == 32 || bitCount == 16);
				name =    bitCount == 64 ? "double"
					: bitCount == 32 ? "float"
					: "half";
			}
			else if(type == eSInt)
			{
				RDCASSERT(bitCount == 64 || bitCount == 32 || bitCount == 16 || bitCount == 8);
				name =    bitCount == 64 ? "long"
					: bitCount == 32 ? "int"
					: bitCount == 16 ? "short"
					: "byte";
			}
			else if(type == eUInt)
			{
				RDCASSERT(bitCount == 64 || bitCount == 32 || bitCount == 16 || bitCount == 8);
				name =    bitCount == 64 ? "ulong"
					: bitCount == 32 ? "uint"
					: bitCount == 16 ? "ushort"
					: "ubyte";
			}
			else if(type == eVector)
			{
				name = StringFormat::Fmt("%s%u", baseType->GetName().c_str(), vectorSize);
			}
			else if(type == eMatrix)
			{
				name = StringFormat::Fmt("%s%ux%u", baseType->GetName().c_str(), vectorSize, matrixSize);
			}
			else if(type == ePointer)
			{
				name = StringFormat::Fmt("%s*", baseType->GetName().c_str());
			}
			else if(type == eImage)
			{
				string typestring = baseType->GetName();
				if(imgformat != spv::ImageFormatUnknown)
					typestring += ", " + ToStr::Get(imgformat);

				name = StringFormat::Fmt("%sImage%s%s%s<%s>",
					depth ? "Depth" : "",
					multisampled ? "MS" : "",
					arrayed ? "Array" : "",
					ToStr::Get(texdim).c_str(),
					typestring.c_str());
			}
			else if(type == eSampledImage)
			{
				name = "Sampled" + baseType->GetName();
			}
			else if(type == eSampler)
			{
				name = "Sampler";
			}
			else
			{
				RDCERR("Unexpected type!");
				name = StringFormat::Fmt("Unhandled_%u_Type", type);
			}
		}

		return name;
	}

	// struct/function
	vector< pair<SPVTypeData *, string> > children;
	vector< vector<SPVDecoration> > decorations; // matches children

	// pointer
	spv::StorageClass storage;

	// sampler/texture/whatever
	spv::Dim texdim;
	uint32_t sampled;
	bool arrayed;
	bool depth;
	bool multisampled;
	spv::ImageFormat imgformat;

	// ints and floats
	uint32_t bitCount;

	uint32_t vectorSize;
	uint32_t matrixSize;
	uint32_t arraySize;
};

struct SPVOperation
{
	SPVOperation() : type(NULL), access(spv::MemoryAccessMaskNone), funcCall(0), complexity(0), mathop(false), inlineArgs(0) {}

	SPVTypeData *type;

	// OpLoad/OpStore/OpCopyMemory
	spv::MemoryAccessMask access;

	// OpExtInst
	vector<uint32_t> literals;

	// OpFunctionCall
	uint32_t funcCall;

	// this is modified on the fly, it's used as a measure of whether we
	// can combine multiple statements into one line when displaying the
	// disassembly.
	int complexity;

	// if this operation will be of the form 'a + b', we need to be sure
	// to bracket any arguments that are mathops in nested expressions,
	// to make order of operations clear.
	bool mathop;

	// bitfield indicating which arguments should be inlined
	uint32_t inlineArgs;

	// arguments always reference IDs that already exist (branch/flow
	// control type statements aren't SPVOperations)
	vector<SPVInstruction*> arguments;

	void GetArg(const vector<SPVInstruction *> &ids, size_t idx, string &arg);
};

struct SPVConstant
{
	SPVConstant() : type(NULL), u64(0) {}

	struct SamplerData
	{
		spv::SamplerAddressingMode addressing;
		bool normalised;
		spv::SamplerFilterMode filter;
	};

	SPVTypeData *type;
	union
	{
		uint64_t u64;
		uint32_t u32;
		uint16_t u16;
		uint8_t u8;
		int64_t i64;
		int32_t i32;
		int16_t i16;
		int8_t i8;
		float f;
		double d;
		SamplerData sampler;
	};

	vector<SPVConstant *> children;

	string GetValString()
	{
		RDCASSERT(children.empty());

		if(type->type == SPVTypeData::eFloat)
		{
			// @ is a custom printf flag that ensures we always print .0
			// after a float, but without restricting precision or sigfigs
			if(type->bitCount == 64)
				return StringFormat::Fmt("%@lgf", d);
			if(type->bitCount == 32)
				return StringFormat::Fmt("%@gf", f);
			if(type->bitCount == 16)
				return StringFormat::Fmt("%@gf", ConvertFromHalf(u16));
		}
		else if(type->type == SPVTypeData::eSInt)
		{
			if(type->bitCount == 64)
				return StringFormat::Fmt("%lli", i64);
			if(type->bitCount == 32)
				return StringFormat::Fmt("%i", i32);
			if(type->bitCount == 16)
				return StringFormat::Fmt("%hi", i16);
			if(type->bitCount == 8)
				return StringFormat::Fmt("%hhi", i8);
		}
		else if(type->type == SPVTypeData::eUInt)
		{
			if(type->bitCount == 64)
				return StringFormat::Fmt("%llu", u64);
			if(type->bitCount == 32)
				return StringFormat::Fmt("%u", u32);
			if(type->bitCount == 16)
				return StringFormat::Fmt("%hu", u16);
			if(type->bitCount == 8)
				return StringFormat::Fmt("%hhu", u8);
		}
		else if(type->type == SPVTypeData::eBool)
			return u32 ? "true" : "false";

		return StringFormat::Fmt("!%u!", u32);
	}

	string GetIDName()
	{
		if(type->IsScalar())
		{
			return GetValString();
		}

		// special case vectors with the same constant
		// replicated across all channels
		if(type->type == SPVTypeData::eVector)
		{
			bool identical = true;
			for(size_t i=1; i < children.size(); i++)
			{
				if(children[i]->u64 != children[0]->u64)
				{
					identical = false;
					break;
				}
			}

			if(identical)
			{
				string ret = children[0]->GetValString() + ".";
				for(size_t i=0; i < children.size(); i++)
					ret += 'x';
				return ret;
			}
		}

		string ret = type->GetName();
		if(type->type == SPVTypeData::eArray)
		{
			ret = type->baseType->GetName();
			ret += StringFormat::Fmt("[%u]", (uint32_t)children.size());
		}
		ret += "(";
		if(children.empty())
			ret += GetValString();
		for(size_t i=0; i < children.size(); i++)
		{
			ret += children[i]->GetIDName();
			if(i+1 < children.size())
			{
				ret += ", ";
				// put each array element on a different line, with some kind of
				// estimated indent (too complex with current blindly-append
				// scheme to match exactly)
				if(type->type == SPVTypeData::eArray)
					ret += "\n                        ";
			}
		}
		ret += ")";

		return ret;
	}
};

struct SPVVariable
{
	SPVVariable() : type(NULL), storage(spv::StorageClassUniformConstant), initialiser(NULL) {}

	SPVTypeData *type;
	spv::StorageClass storage;
	SPVConstant *initialiser;
};

struct SPVFlowControl
{
	SPVFlowControl() : selControl(spv::SelectionControlMaskNone), condition(NULL) {}

	union
	{
		spv::SelectionControlMask selControl;
		spv::LoopControlMask loopControl;
	};
	
	SPVInstruction *condition;

	// branch weights or switch cases
	vector<uint32_t> literals;

	// flow control can reference future IDs, so we index
	vector<uint32_t> targets;
};

struct SPVBlock
{
	SPVBlock() : mergeFlow(NULL), exitFlow(NULL) {}
	
	vector<SPVInstruction *> instructions;

	SPVInstruction *mergeFlow;
	SPVInstruction *exitFlow;
};

struct SPVFunction
{
	SPVFunction() : retType(NULL), funcType(NULL), control(spv::FunctionControlMaskNone) {}

	SPVTypeData *retType;
	SPVTypeData *funcType;
	vector<SPVInstruction *> arguments;

	spv::FunctionControlMask control;

	vector<SPVInstruction *> blocks;
	vector<SPVInstruction *> variables;
};

struct SPVInstruction
{
	SPVInstruction()
	{
		opcode = spv::OpNop;
		id = 0;

		ext = NULL;
		entry = NULL;
		op = NULL;
		flow = NULL;
		type = NULL;
		func = NULL;
		block = NULL;
		constant = NULL;
		var = NULL;

		line = -1;

		source.col = source.line = 0;
	}

	~SPVInstruction()
	{
		SAFE_DELETE(ext);
		SAFE_DELETE(entry);
		SAFE_DELETE(op);
		SAFE_DELETE(flow);
		SAFE_DELETE(type);
		SAFE_DELETE(func);
		SAFE_DELETE(block);
		SAFE_DELETE(constant);
		SAFE_DELETE(var);
	}

	spv::Op opcode;
	uint32_t id;

	// line number in disassembly (used for stepping when debugging)
	int line;

	struct { string filename; uint32_t line; uint32_t col; } source;

	string str;

	const string &GetIDName()
	{
		if(str.empty())
		{
			if(constant)
				str = constant->GetIDName();
			else
				str = DefaultIDName(id);
		}

		return str;
	}

	string Disassemble(const vector<SPVInstruction *> &ids, bool inlineOp)
	{
		switch(opcode)
		{
			case spv::OpConstant:
			case spv::OpConstantComposite:
			case spv::OpVariable:
			case spv::OpFunctionParameter:
			{
				return GetIDName();
			}
			case spv::OpLabel:
			{
				RDCASSERT(!inlineOp);
				return StringFormat::Fmt("Label%u:", id);
			}
			case spv::OpReturn:
			{
				RDCASSERT(!inlineOp);
				return "Return";
			}
			case spv::OpReturnValue:
			{
				RDCASSERT(!inlineOp);

				string arg = ids[flow->targets[0]]->Disassemble(ids, true);

				return StringFormat::Fmt("Return %s", arg.c_str());
			}
			case spv::OpBranch:
			{
				RDCASSERT(!inlineOp);
				return StringFormat::Fmt("goto Label%u", flow->targets[0]);
			}
			case spv::OpBranchConditional:
			{
				RDCASSERT(!inlineOp);

				// we don't output the targets since that is handled specially

				if(flow->literals.empty())
					return flow->condition->Disassemble(ids, true);

				uint32_t weightA = flow->literals[0];
				uint32_t weightB = flow->literals[1];

				float a = float(weightA)/float(weightA+weightB);
				float b = float(weightB)/float(weightA+weightB);

				a *= 100.0f;
				b *= 100.0f;

				return StringFormat::Fmt("%s [true: %.2f%%, false: %.2f%%]", flow->condition->Disassemble(ids, true).c_str(), a, b);
			}
			case spv::OpSelectionMerge:
			{
				RDCASSERT(!inlineOp);
				return StringFormat::Fmt("SelectionMerge Label%u%s", flow->targets[0], OptionalFlagString(flow->selControl).c_str());
			}
			case spv::OpLoopMerge:
			{
				RDCASSERT(!inlineOp);
				return StringFormat::Fmt("LoopMerge Label%u%s", flow->targets[0], OptionalFlagString(flow->loopControl).c_str());
			}
			case spv::OpStore:
			{
				RDCASSERT(op);

				string dest, src;
				op->GetArg(ids, 0, dest);
				op->GetArg(ids, 1, src);

				// inlined only in function parameters, just return argument
				if(inlineOp)
					return src;

				char assignStr[] = " = ";

				if(op->arguments[1]->opcode == spv::OpCompositeInsert && (op->inlineArgs & 2))
					assignStr[0] = 0;
				
#if LOAD_STORE_CONSTRUCTORS
				return StringFormat::Fmt("Store(%s%s)%s%s", dest.c_str(), OptionalFlagString(op->access).c_str(), assignStr, src.c_str());
#else
				return StringFormat::Fmt("%s%s%s%s", dest.c_str(), OptionalFlagString(op->access).c_str(), assignStr, src.c_str());
#endif
			}
			case spv::OpCopyMemory:
			{
				RDCASSERT(!inlineOp && op);
				
				string dest, src;
				op->GetArg(ids, 0, dest);
				op->GetArg(ids, 1, src);

#if LOAD_STORE_CONSTRUCTORS
				return StringFormat::Fmt("Copy(%s%s) = Load(%s%s)", dest.c_str(), OptionalFlagString(op->access).c_str(), src.c_str(), OptionalFlagString(op->access).c_str());
#else
				return StringFormat::Fmt("%s%s = %s%s", dest.c_str(), OptionalFlagString(op->access).c_str(), src.c_str(), OptionalFlagString(op->access).c_str());
#endif
			}
			case spv::OpLoad:
			{
				RDCASSERT(op);

				string arg;
				op->GetArg(ids, 0, arg);
				
#if LOAD_STORE_CONSTRUCTORS
				if(inlineOp)
					return StringFormat::Fmt("Load(%s%s)", arg.c_str(), OptionalFlagString(op->access).c_str());

				return StringFormat::Fmt("%s %s = Load(%s%s)", op->type->GetName().c_str(), GetIDName().c_str(), arg.c_str(), OptionalFlagString(op->access).c_str());
#else
				if(inlineOp)
					return StringFormat::Fmt("%s%s", arg.c_str(), OptionalFlagString(op->access).c_str());

				return StringFormat::Fmt("%s %s = %s%s", op->type->GetName().c_str(), GetIDName().c_str(), arg.c_str(), OptionalFlagString(op->access).c_str());
#endif
			}
			case spv::OpCompositeConstruct:
			{
				RDCASSERT(op);

				string ret = "";
				
				if(!inlineOp)
					ret = StringFormat::Fmt("%s %s = ", op->type->GetName().c_str(), GetIDName().c_str());

				ret += op->type->GetName();
				ret += "(";
				
				for(size_t i=0; i < op->arguments.size(); i++)
				{
					string constituent;
					op->GetArg(ids, i, constituent);

					ret += constituent;
					if(i+1 < op->arguments.size())
						ret += ", ";
				}

				ret += ")";

				return ret;
			}
			case spv::OpCompositeExtract:
			case spv::OpCompositeInsert:
			case spv::OpAccessChain:
			{
				RDCASSERT(op);
				
				string composite;
				op->GetArg(ids, 0, composite);

				// unknown argument, we can't access chain it
				if(op->arguments[0]->var == NULL && op->arguments[0]->op == NULL)
				{
					string ret = "";

					if(!inlineOp)
						ret = StringFormat::Fmt("%s %s = ", op->type->GetName().c_str(), GetIDName().c_str());

					ret += composite + "....";

					return ret;
				}

				SPVTypeData *type = op->arguments[0]->var ? op->arguments[0]->var->type : op->arguments[0]->op->type;

				RDCASSERT(type);

				if(type->type == SPVTypeData::ePointer)
					type = type->baseType;

				size_t start = (opcode == spv::OpAccessChain ? 1                    : 0                  );
				size_t count = (opcode == spv::OpAccessChain ? op->arguments.size() : op->literals.size());

				string accessString = "";

				for(size_t i=start; i < count; i++)
				{
					bool constant = false;
					int32_t idx = -1;
					if(opcode != spv::OpAccessChain)
					{
						idx = (int32_t)op->literals[i];
						constant = true;
					}
					else if(op->arguments[i]->constant)
					{
						RDCASSERT(op->arguments[i]->constant && op->arguments[i]->constant->type->IsBasicInt());
						idx = op->arguments[i]->constant->i32;
						constant = true;
					}

					if(!type)
						break;

					if(type->type == SPVTypeData::eStruct)
					{
						// Assuming you can't dynamically index into a structure
						RDCASSERT(constant);
						const pair<SPVTypeData*,string> &child = type->children[idx];
						if(child.second.empty())
							accessString += StringFormat::Fmt("._member%u", idx);
						else
							accessString += StringFormat::Fmt(".%s", child.second.c_str());
						type = child.first;
						continue;
					}
					else if(type->type == SPVTypeData::eArray)
					{
						if(constant)
						{
							accessString += StringFormat::Fmt("[%u]", idx);
						}
						else
						{
							// dynamic indexing into this array
							string arg;
							op->GetArg(ids, i, arg);
							accessString += StringFormat::Fmt("[%s]", arg.c_str());
						}
						type = type->baseType;
						continue;
					}
					else if(type->type == SPVTypeData::eMatrix)
					{
						if(constant)
						{
							accessString += StringFormat::Fmt("[%u]", idx);
						}
						else
						{
							// dynamic indexing into this array
							string arg;
							op->GetArg(ids, i, arg);
							accessString += StringFormat::Fmt("[%s]", arg.c_str());
						}

						// fall through to vector if we have another index
						if(i == count-1)
							break;

						i++;
						
						if(opcode != spv::OpAccessChain)
						{
							idx = (int32_t)op->literals[i];
						}
						else
						{
							// assuming can't dynamically index into a vector (would be a OpVectorShuffle)
							RDCASSERT(op->arguments[i]->constant && op->arguments[i]->constant->type->IsBasicInt());
							idx = op->arguments[i]->constant->i32;
						}
					}

					// vector (or matrix + extra)
					{
						char swizzle[] = "xyzw";
						if(idx < 4)
							accessString += StringFormat::Fmt(".%c", swizzle[idx]);
						else
							accessString += StringFormat::Fmt("._%u", idx);

						// must be the last index, we're down to scalar granularity
						type = NULL;
						RDCASSERT(i == count-1);
					}
				}
				
				string ret = "";

				if(opcode == spv::OpCompositeInsert)
				{
					string insertObj;
					op->GetArg(ids, 1, insertObj);

					// if we've been inlined, it means that there is a store of the result of
					// this composite insert, to the same composite that we are cloning (first
					// argument). If so, we can just leave the access and object assignment
					if(inlineOp)
					{
						ret = StringFormat::Fmt("%s = %s", accessString.c_str(), insertObj.c_str());
					}
					else
					{
						ret = StringFormat::Fmt("%s %s = %s; %s%s = %s",
							op->type->GetName().c_str(), GetIDName().c_str(),
							composite.c_str(),
							GetIDName().c_str(), accessString.c_str(),
							insertObj.c_str()
							);
					}
				}
				else
				{
					if(!inlineOp)
						ret = StringFormat::Fmt("%s %s = ", op->type->GetName().c_str(), GetIDName().c_str());

					ret += composite + accessString;
				}

				return ret;
			}
			case spv::OpExtInst:
			{
				RDCASSERT(op);

				string ret = "";

				if(!inlineOp)
					ret = StringFormat::Fmt("%s %s = ", op->type->GetName().c_str(), GetIDName().c_str());

				ret += op->arguments[0]->ext->setname + "::";
				ret += op->arguments[0]->ext->instructions[op->literals[0]];
				ret += "(";

				for(size_t i=1; i < op->arguments.size(); i++)
				{
					string arg;
					op->GetArg(ids, i, arg);

					ret += arg;
					if(i+1 < op->arguments.size())
						ret += ", ";
				}

				ret += ")";

				return ret;
			}
			// texture samples almost identical to function call
			case spv::OpImageSampleImplicitLod:
			case spv::OpImageSampleExplicitLod:
			// conversions can be treated the same way
			case spv::OpConvertFToS:
			case spv::OpConvertFToU:
			case spv::OpConvertUToF:
			case spv::OpConvertSToF:
			case spv::OpBitcast:
			case spv::OpFunctionCall:
			{
				RDCASSERT(op);

				string ret = "";

				if(!inlineOp && op->type->type != SPVTypeData::eVoid)
					ret = StringFormat::Fmt("%s %s = ", op->type->GetName().c_str(), GetIDName().c_str());

				if(opcode == spv::OpFunctionCall)
					ret += ids[op->funcCall]->GetIDName() + "(";
				else if(opcode == spv::OpBitcast)
					ret += "Bitcast<" + op->type->GetName() + ">(";
				else
					ret += ToStr::Get(opcode) + "(";

				for(size_t i=0; i < op->arguments.size(); i++)
				{
					string arg;
					op->GetArg(ids, i, arg);

					ret += arg;
					if(i+1 < op->arguments.size())
						ret += ", ";
				}

				ret += ")";

				return ret;
			}
			case spv::OpVectorShuffle:
			{
				RDCASSERT(op);

				string ret = "";

				if(!inlineOp)
					ret = StringFormat::Fmt("%s %s = ", op->type->GetName().c_str(), GetIDName().c_str());

				SPVTypeData *vec1type = op->arguments[0]->op->type;
				SPVTypeData *vec2type = op->arguments[1]->constant ? op->arguments[1]->constant->type : op->arguments[1]->op->type;

				RDCASSERT(vec1type->type == SPVTypeData::eVector && vec2type->type == SPVTypeData::eVector);

				uint32_t maxShuffle = 0;
				for(size_t i=0; i < op->literals.size(); i++)
				{
					uint32_t s = op->literals[i];
					if(s > vec1type->vectorSize)
						s -= vec1type->vectorSize;
					maxShuffle = RDCMAX(maxShuffle, op->literals[i]);
				}

				ret += op->type->GetName() + "(";

				// sane path for 4-vectors or less
				if(maxShuffle < 4)
				{
					char swizzle[] = "xyzw_";

					int lastvec = -1;
					for(size_t i=0; i < op->literals.size(); i++)
					{
						int vec = 0;
						uint32_t s = op->literals[i];
						if(s == 0xFFFFFFFF)
						{
							// undefined component
							s = 4;
						}
						else if(s > vec1type->vectorSize)
						{
							s -= vec1type->vectorSize;
							vec = 1;
						}

						if(vec != lastvec)
						{
							lastvec = vec;
							if(i > 0)
								ret += ", ";
							string arg;
							op->GetArg(ids, 0, arg);
							ret += arg;
							ret += ".";
						}

						ret += swizzle[s];
					}
				}

				ret += ")";

				return ret;
			}
			case spv::OpFNegate:
			case spv::OpNot:
			case spv::OpLogicalNot:
			{
				// unary math operation
				RDCASSERT(op);

				char c = '?';
				switch(opcode)
				{
					case spv::OpFNegate:
						c = '-';
						break;
					case spv::OpNot:
						c = '~';
						break;
					case spv::OpLogicalNot:
						c = '!';
						break;
					default:
						break;
				}

				string a;
				op->GetArg(ids, 0, a);

				if(op->arguments[0]->op && op->arguments[0]->op->mathop)
					a = "(" + a + ")";

				if(inlineOp)
					return StringFormat::Fmt("%c%s", c, a.c_str());

				return StringFormat::Fmt("%s %s = %c%s", op->type->GetName().c_str(), GetIDName().c_str(), c, a.c_str());
			}
			case spv::OpIAdd:
			case spv::OpFAdd:
			case spv::OpISub:
			case spv::OpFSub:
			case spv::OpIMul:
			case spv::OpFMul:
			case spv::OpFDiv:
			case spv::OpFMod:
			case spv::OpVectorTimesScalar:
			case spv::OpMatrixTimesVector:
			case spv::OpMatrixTimesMatrix:
			case spv::OpSLessThan:
			case spv::OpSLessThanEqual:
			case spv::OpFOrdLessThan:
			case spv::OpFOrdGreaterThan:
			case spv::OpFOrdGreaterThanEqual:
			case spv::OpLogicalAnd:
			case spv::OpLogicalOr:
			case spv::OpLogicalNotEqual:
			case spv::OpShiftLeftLogical:
			{
				// binary math operation
				RDCASSERT(op);

				char opstr[3] = { '?', 0, 0 };
				switch(opcode)
				{
					case spv::OpIAdd:
					case spv::OpFAdd:
						opstr[0] = '+';
						break;
					case spv::OpISub:
					case spv::OpFSub:
						opstr[0] = '-';
						break;
					case spv::OpIMul:
					case spv::OpFMul:
					case spv::OpVectorTimesScalar:
					case spv::OpMatrixTimesVector:
					case spv::OpMatrixTimesMatrix:
						opstr[0] = '*';
						break;
					case spv::OpSLessThan:
					case spv::OpFOrdLessThan:
						opstr[0] = '<';
						break;
					case spv::OpSLessThanEqual:
						opstr[0] = '<'; opstr[1] = '=';
						break;
					case spv::OpFOrdGreaterThan:
						opstr[0] = '>';
						break;
					case spv::OpFOrdGreaterThanEqual:
						opstr[0] = '>'; opstr[1] = '=';
						break;
					case spv::OpFDiv:
						opstr[0] = '/';
						break;
					case spv::OpFMod:
						opstr[0] = '%';
						break;
					case spv::OpLogicalAnd:
						opstr[0] = opstr[1] = '&';
						break;
					case spv::OpLogicalOr:
						opstr[0] = opstr[1] = '|';
						break;
					case spv::OpLogicalNotEqual:
						opstr[0] = '!'; opstr[1] = '=';
						break;
					case spv::OpShiftLeftLogical:
						opstr[0] = '<'; opstr[1] = '<';
						break;
					default:
						break;
				}

				string a, b;
				op->GetArg(ids, 0, a);
				op->GetArg(ids, 1, b);

				if(op->arguments[0]->op && op->arguments[0]->op->mathop)
					a = "(" + a + ")";
				if(op->arguments[1]->op && op->arguments[1]->op->mathop)
					b = "(" + b + ")";

				if(inlineOp)
					return StringFormat::Fmt("%s %s %s", a.c_str(), opstr, b.c_str());

				return StringFormat::Fmt("%s %s = %s %s %s", op->type->GetName().c_str(), GetIDName().c_str(), a.c_str(), opstr, b.c_str());
			}
			case spv::OpDot:
			{
				// binary math function
				RDCASSERT(op);

				string a, b;
				op->GetArg(ids, 0, a);
				op->GetArg(ids, 1, b);

				if(inlineOp)
					return StringFormat::Fmt("%s(%s, %s)", ToStr::Get(opcode).c_str(), a.c_str(), b.c_str());

				return StringFormat::Fmt("%s %s = %s(%s, %s)", op->type->GetName().c_str(), GetIDName().c_str(), ToStr::Get(opcode).c_str(), a.c_str(), b.c_str());
			}
			case spv::OpSelect:
			{
				RDCASSERT(op);

				string a, b, c;
				op->GetArg(ids, 0, a);
				op->GetArg(ids, 1, b);
				op->GetArg(ids, 2, c);

				if(inlineOp)
					return StringFormat::Fmt("(%s) ? (%s) : (%s)", a.c_str(), b.c_str(), c.c_str());

				return StringFormat::Fmt("%s %s = (%s) ? (%s) : (%s)", op->type->GetName().c_str(), GetIDName().c_str(), a.c_str(), b.c_str(), c.c_str());
			}
			default:
				break;
		}

		if(opcode == spv::OpUnknown)
		{
			// we don't know where this ID came from, this is a dummy op
			return "UnknownOp(" + GetIDName() + ")";
		}

		// fallback for operations that we don't disassemble
		string ret = "!!";

		if(!str.empty() || id != 0)
			ret += GetIDName() + " <= ";

		ret = ToStr::Get(opcode) + "(";
		for(size_t a=0; op && a < op->arguments.size(); a++)
		{
			ret += op->arguments[a]->GetIDName();
			if(a+1 < op->arguments.size())
				ret += ", ";
		}
		ret += ")";

		return ret;
	}

	vector<SPVDecoration> decorations;

	// zero or one of these pointers might be set
	SPVExtInstSet *ext; // this ID is an extended instruction set
	SPVEntryPoint *entry; // this ID is an entry point
	SPVOperation *op; // this ID is the result of an operation
	SPVFlowControl *flow; // this is a flow control operation (no ID)
	SPVTypeData *type; // this ID names a type
	SPVFunction *func; // this ID names a function
	SPVBlock *block; // this is the ID of a label
	SPVConstant *constant; // this ID is a constant value
	SPVVariable *var; // this ID is a variable
};

void SPVOperation::GetArg(const vector<SPVInstruction *> &ids, size_t idx, string &arg)
{
	if(inlineArgs & (1<<idx))
		arg = arguments[idx]->Disassemble(ids, true);
	else
		arg = arguments[idx]->GetIDName();
}

static bool IsUnmodified(SPVFunction *func, SPVInstruction *from, SPVInstruction *to)
{
	// if it's not a variable (e.g. constant or something), just return true,
	// it's pure.
	if(from->op == NULL) return true;
	
	// if we're looking at a load of a variable, ensure that it's pure
	if(from->opcode == spv::OpLoad && from->op->arguments[0]->var)
	{
		SPVInstruction *var = from->op->arguments[0];

		bool looking = false;
		bool done = false;

		for(size_t b=0; b < func->blocks.size(); b++)
		{
			SPVInstruction *block = func->blocks[b];

			for(size_t i=0; i < block->block->instructions.size(); i++)
			{
				SPVInstruction *instr = block->block->instructions[i];
				if(instr == from)
				{
					looking = true;
				}
				else if(instr == to)
				{
					looking = false;
					done = true;
					break;
				}
				else if(looking && instr->opcode == spv::OpStore && instr->op->arguments[0] == var)
				{
					return false;
				}
			}

			if(done)
				break;
		}

		return true;
	}

	// otherwise, recurse
	bool ret = true;

	for(size_t i=0; i < from->op->arguments.size(); i++)
	{
		if(from->opcode == spv::OpStore && i == 0)
			continue;

		// this operation is pure if all of its arguments are pure up to the point
		// of use
		ret &= IsUnmodified(func, from->op->arguments[i], to);
	}

	return ret;
}

SPVModule::SPVModule()
{
	moduleVersion = 0;
	generator = 0;
	sourceVer = 0;
}

SPVModule::~SPVModule()
{
	for(size_t i=0; i < operations.size(); i++)
		delete operations[i];
	operations.clear();
}

SPVInstruction * SPVModule::GetByID(uint32_t id)
{
	if(ids[id]) return ids[id];

	// if there's an unrecognised instruction (e.g. from an extension) that generates
	// an ID, it won't be in our list so we have to add a dummy instruction for it
	RDCWARN("Expected to find ID %u but didn't - returning dummy instruction", id);

	operations.push_back(new SPVInstruction());
	SPVInstruction &op = *operations.back();
	op.opcode = spv::OpUnknown;
	op.id = id;
	ids[id] = &op;

	return &op;
}

void SPVModule::Disassemble()
{
	m_Disassembly = "SPIR-V:\n\n";

	const char *gen = "Unrecognised";

	for(size_t i=0; i < ARRAY_COUNT(KnownGenerators); i++) if(KnownGenerators[i].magic == generator)	gen = KnownGenerators[i].name;

	m_Disassembly += StringFormat::Fmt("Version %u, Generator %08x (%s)\n", moduleVersion, generator, gen);
	m_Disassembly += StringFormat::Fmt("IDs up to {%u}\n", (uint32_t)ids.size());

	m_Disassembly += "\n";

	m_Disassembly += StringFormat::Fmt("Source is %s %u\n", ToStr::Get(sourceLang).c_str(), sourceVer);
	for(size_t s=0; s < sourceexts.size(); s++)
		m_Disassembly += StringFormat::Fmt(" + %s\n", sourceexts[s]->str.c_str());

	m_Disassembly += "\n";

	m_Disassembly += "Capabilities:";
	for(size_t c=0; c < capabilities.size(); c++)
		m_Disassembly += StringFormat::Fmt(" %s", ToStr::Get(capabilities[c]).c_str());
	m_Disassembly += "\n";

	for(size_t i=0; i < entries.size(); i++)
	{
		RDCASSERT(entries[i]->entry);
		uint32_t func = entries[i]->entry->func;
		RDCASSERT(ids[func]);
		m_Disassembly += StringFormat::Fmt("Entry point '%s' (%s)\n", ids[func]->str.c_str(), ToStr::Get(entries[i]->entry->model).c_str());

		for(size_t m=0; m < entries[i]->entry->modes.size(); m++)
		{
			SPVExecutionMode &mode = entries[i]->entry->modes[m];
			m_Disassembly += StringFormat::Fmt("            %s", ToStr::Get(mode.mode).c_str());
			if(mode.mode == spv::ExecutionModeInvocations || mode.mode == spv::ExecutionModeOutputVertices)
				m_Disassembly += StringFormat::Fmt(" = %u", mode.x);
			if(mode.mode == spv::ExecutionModeLocalSize || mode.mode == spv::ExecutionModeLocalSizeHint)
				m_Disassembly += StringFormat::Fmt(" = <%u, %u, %u>", mode.x, mode.y, mode.z);
			if(mode.mode == spv::ExecutionModeVecTypeHint)
			{
				uint16_t dataType = (mode.x & 0xffff);
				uint16_t numComps = (mode.y >> 16) & 0xffff;
				switch(dataType)
				{
					// 0 represents an 8-bit integer value.
					case 0:  m_Disassembly += StringFormat::Fmt(" = byte%u", numComps); break;
					// 1 represents a 16-bit integer value.
					case 1:  m_Disassembly += StringFormat::Fmt(" = short%u", numComps); break;
					// 2 represents a 32-bit integer value.
					case 2:  m_Disassembly += StringFormat::Fmt(" = int%u", numComps); break;
					// 3 represents a 64-bit integer value.
					case 3:  m_Disassembly += StringFormat::Fmt(" = longlong%u", numComps); break;
					// 4 represents a 16-bit float value.
					case 4:  m_Disassembly += StringFormat::Fmt(" = half%u", numComps); break;
					// 5 represents a 32-bit float value.
					case 5:  m_Disassembly += StringFormat::Fmt(" = float%u", numComps); break;
					// 6 represents a 64-bit float value.
					case 6:  m_Disassembly += StringFormat::Fmt(" = double%u", numComps); break;
					// ...
					default: m_Disassembly += StringFormat::Fmt(" = invalid%u", numComps); break;
				}
			}

			m_Disassembly += "\n";
		}
	}

	m_Disassembly += "\n";

	for(size_t i=0; i < structs.size(); i++)
	{
		m_Disassembly += StringFormat::Fmt("struct %s {\n", structs[i]->type->GetName().c_str());
		for(size_t c=0; c < structs[i]->type->children.size(); c++)
		{
			auto member = structs[i]->type->children[c];

			string varName = member.second;

			if(varName.empty())
				varName = StringFormat::Fmt("member%u", c);

			m_Disassembly += StringFormat::Fmt("  %s;\n", member.first->DeclareVariable(structs[i]->type->decorations[c], varName).c_str());
		}
		m_Disassembly += StringFormat::Fmt("}; // struct %s\n\n", structs[i]->type->GetName().c_str());
	}

	for(size_t i=0; i < globals.size(); i++)
	{
		RDCASSERT(globals[i]->var && globals[i]->var->type);

		// if name is set to blank, inherit it from the underlying type
		// we set this to the variable name, so it can be used in subsequent ops
		// referring to this global.
		if(globals[i]->str.empty())
		{
			if(globals[i]->var && !globals[i]->var->type->name.empty())
				globals[i]->str = globals[i]->var->type->name;
			else if(globals[i]->var && globals[i]->var->type->type == SPVTypeData::ePointer && !globals[i]->var->type->baseType->name.empty())
				globals[i]->str = globals[i]->var->type->baseType->name;
		}

		string varName = globals[i]->str;
		m_Disassembly += StringFormat::Fmt("%s %s;\n", ToStr::Get(globals[i]->var->storage).c_str(), globals[i]->var->type->DeclareVariable(globals[i]->decorations, varName).c_str());
	}

	m_Disassembly += "\n";

	for(size_t f=0; f < funcs.size(); f++)
	{
		SPVFunction *func = funcs[f]->func;
		RDCASSERT(func && func->retType && func->funcType);

		string args = "";

		for(size_t a=0; a < func->funcType->children.size(); a++)
		{
			const pair<SPVTypeData *,string> &arg = func->funcType->children[a];
			RDCASSERT(a < func->arguments.size());
			const SPVInstruction *argname = func->arguments[a];

			if(argname->str.empty())
				args += arg.first->GetName();
			else
				args += StringFormat::Fmt("%s %s", arg.first->GetName().c_str(), argname->str.c_str());

			if(a+1 < func->funcType->children.size())
				args += ", ";
		}

		m_Disassembly += StringFormat::Fmt("%s %s(%s)%s {\n", func->retType->GetName().c_str(), funcs[f]->str.c_str(), args.c_str(), OptionalFlagString(func->control).c_str());

		// local copy of variables vector
		vector<SPVInstruction *> vars = func->variables;
		vector<SPVInstruction *> funcops;

		for(size_t b=0; b < func->blocks.size(); b++)
		{
			SPVInstruction *block = func->blocks[b];

			// don't push first label in a function
			if(b > 0)
				funcops.push_back(block); // OpLabel

			set<SPVInstruction *> ignore_items;

			for(size_t i=0; i < block->block->instructions.size(); i++)
			{
				SPVInstruction *instr = block->block->instructions[i];

				if(ignore_items.find(instr) == ignore_items.end())
					funcops.push_back(instr);

				if(instr->op)
				{
					int maxcomplex = 0;

					for(size_t a=0; a < instr->op->arguments.size(); a++)
					{
						SPVInstruction *arg = instr->op->arguments[a];

						if(arg->op)
						{
							// allow less inlining in composite constructs
							int maxAllowedComplexity = NO_INLINE_COMPLEXITY;
							if(instr->opcode == spv::OpCompositeConstruct)
								maxAllowedComplexity = RDCMIN(2, maxAllowedComplexity);

							// don't fold up too complex an operation
							// allow some ops to have multiple arguments, others with many
							// arguments should not be inlined
							if(arg->op->complexity >= maxAllowedComplexity ||
									(arg->op->arguments.size() > 2 &&
									 arg->opcode != spv::OpAccessChain &&
									 arg->opcode != spv::OpSelect &&
									 arg->opcode != spv::OpCompositeConstruct))
								continue;

							// for anything but store's dest argument
							if(instr->opcode != spv::OpStore || a > 0)
							{
								// Do not inline this argument if it relies on a load from a
								// variable that is written to between the argument and this
								// op that we're inlining into, as that changes the meaning.
								if(!IsUnmodified(func, arg, instr))
									continue;
							}

							maxcomplex = RDCMAX(arg->op->complexity, maxcomplex);
						}

						erase_item(funcops, arg);

						instr->op->inlineArgs |= (1<<a);
					}

					instr->op->complexity = maxcomplex;
					
					if(instr->opcode != spv::OpStore &&
						instr->opcode != spv::OpLoad &&
						instr->opcode != spv::OpCompositeExtract &&
						instr->op->inlineArgs)
						instr->op->complexity++;

					// we try to merge away temp variables that are only used for a single store then a single
					// load later. We can only do this if:
					//  - The Load we're looking is the only load in this function of the variable
					//  - The Load is preceeded by precisely one Store - not 0 or 2+
					//  - The previous store is 'pure', ie. does not depend on any mutated variables
					//    so it is safe to re-order to where the Load is.
					//
					// If those conditions are met then we can remove the previous store, inline it as the load
					// function argument (instead of the variable), and remove the variable.

					if(instr->opcode == spv::OpLoad && funcops.size() > 1)
					{
						SPVInstruction *prevstore = NULL;
						int storecount = 0;

						for(size_t o=0; o < funcops.size(); o++)
						{
							SPVInstruction *previnstr = funcops[o];
							if(previnstr->opcode == spv::OpStore && previnstr->op->arguments[0] == instr->op->arguments[0])
							{
								prevstore = previnstr;
								storecount++;
								if(storecount > 1)
									break;
							}
						}

						if(storecount == 1 && IsUnmodified(func, prevstore, instr))
						{
							bool otherload = false;

							// note variables have function scope, need to check all blocks in this function
							for(size_t o=0; o < func->blocks.size(); o++)
							{
								SPVInstruction *otherblock = func->blocks[o];

								for(size_t l=0; l < otherblock->block->instructions.size(); l++)
								{
									SPVInstruction *otherinstr = otherblock->block->instructions[l];
									if(otherinstr != instr &&
										otherinstr->opcode == spv::OpLoad &&
										otherinstr->op->arguments[0] == instr->op->arguments[0])
									{
										otherload = true;
										break;
									}
								}
							}

							if(!otherload)
							{
								instr->op->complexity = RDCMAX(instr->op->complexity, prevstore->op->complexity);
								erase_item(vars, instr->op->arguments[0]);
								erase_item(funcops, prevstore);
								instr->op->arguments[0] = prevstore;
							}
						}
					}

					// if we have a store from a temp ID, immediately following the op
					// that produced that temp ID, we can combine these trivially
					if((instr->opcode == spv::OpStore || instr->opcode == spv::OpCompositeInsert) && funcops.size() > 1)
					{
						if(instr->op->arguments[1] == funcops[funcops.size()-2])
						{
							erase_item(funcops, instr->op->arguments[1]);
							if(instr->op->arguments[1]->op)
								instr->op->complexity = RDCMAX(instr->op->complexity, instr->op->arguments[1]->op->complexity);
							instr->op->inlineArgs |= 2;
						}
					}

					// special handling for function call to inline temporary pointer variables
					// created for passing parameters
					if(instr->opcode == spv::OpFunctionCall)
					{
						for(size_t a=0; a < instr->op->arguments.size(); a++)
						{
							SPVInstruction *arg = instr->op->arguments[a];

							// if this argument has
							//  - only one usage as a store target before the function call
							//  = then it's an in parameter, and we can fold it in.
							//
							//  - only one usage as a load target after the function call
							//  = then it's an out parameter, we can fold it in as long as
							//    the usage after is in a Store(a) = Load(param) case
							//
							//  - exactly one usage as store before, and load after, such that
							//    it is Store(param) = Load(a) .... Store(a) = Load(param)
							//  = then it's an inout parameter, and we can fold it in

							bool canReplace = true;
							SPVInstruction *storeBefore = NULL;
							SPVInstruction *loadAfter = NULL;
							size_t storeIdx = block->block->instructions.size();
							size_t loadIdx = block->block->instructions.size();

							for(size_t j=0; j < i; j++)
							{
								SPVInstruction *searchInst = block->block->instructions[j];
								for(size_t aa=0; searchInst->op && aa < searchInst->op->arguments.size(); aa++)
								{
									if(searchInst->op->arguments[aa]->id == arg->id)
									{
										if(searchInst->opcode == spv::OpStore)
										{
											// if it's used in multiple stores, it can't be folded
											if(storeBefore)
											{
												canReplace = false;
												break;
											}
											storeBefore = searchInst;
											storeIdx = j;
										}
										else
										{
											// if it's used in anything but a store, it can't be folded
											canReplace = false;
											break;
										}
									}
								}

								// if it's used in a condition, it can't be folded
								if(searchInst->flow && searchInst->flow->condition && searchInst->flow->condition->id == arg->id)
									canReplace = false;

								if(!canReplace)
									break;
							}

							for(size_t j=i+1; j < block->block->instructions.size(); j++)
							{
								SPVInstruction *searchInst = block->block->instructions[j];
								for(size_t aa=0; searchInst->op && aa < searchInst->op->arguments.size(); aa++)
								{
									if(searchInst->op->arguments[aa]->id == arg->id)
									{
										if(searchInst->opcode == spv::OpLoad)
										{
											// if it's used in multiple load, it can't be folded
											if(loadAfter)
											{
												canReplace = false;
												break;
											}
											loadAfter = searchInst;
											loadIdx = j;
										}
										else
										{
											// if it's used in anything but a load, it can't be folded
											canReplace = false;
											break;
										}
									}
								}

								// if it's used in a condition, it can't be folded
								if(searchInst->flow && searchInst->flow->condition && searchInst->flow->condition->id == arg->id)
									canReplace = false;

								if(!canReplace)
									break;
							}

							if(canReplace)
							{
								// in parameter
								if(storeBefore && !loadAfter)
								{
									erase_item(funcops, storeBefore);

									erase_item(vars, instr->op->arguments[a]);

									// pass function parameter directly from where the store was coming from
									instr->op->arguments[a] = storeBefore->op->arguments[1];
								}

								// out or inout parameter
								if(loadAfter)
								{
									// need to check the load afterwards is only ever used in a store operation

									SPVInstruction *storeUse = NULL;

									for(size_t j=loadIdx+1; j < block->block->instructions.size(); j++)
									{
										SPVInstruction *searchInst = block->block->instructions[j];

										for(size_t aa=0; searchInst->op && aa < searchInst->op->arguments.size(); aa++)
										{
											if(searchInst->op->arguments[aa] == loadAfter)
											{
												if(searchInst->opcode == spv::OpStore)
												{
													// if it's used in multiple stores, it can't be folded
													if(storeUse)
													{
														canReplace = false;
														break;
													}
													storeUse = searchInst;
												}
												else
												{
													// if it's used in anything but a store, it can't be folded
													canReplace = false;
													break;
												}
											}
										}

										// if it's used in a condition, it can't be folded
										if(searchInst->flow && searchInst->flow->condition == loadAfter)
											canReplace = false;

										if(!canReplace)
											break;
									}

									if(canReplace && storeBefore != NULL)
									{
										// for the inout parameter case, we also need to verify that
										// the Store() before the function call comes from a Load(),
										// and that the variable being Load()'d is identical to the
										// variable in the Store() in storeUse that we've found

										if(storeBefore->op->arguments[1]->opcode == spv::OpLoad &&
											storeBefore->op->arguments[1]->op->arguments[0]->id ==
											storeUse->op->arguments[0]->id)
										{
											erase_item(funcops, storeBefore);
										}
										else
										{
											canReplace = false;
										}
									}

									if(canReplace)
									{
										// we haven't reached this store instruction yet, so need to mark that
										// it has been folded and should be skipped
										ignore_items.insert(storeUse);

										erase_item(vars, instr->op->arguments[a]);

										// pass argument directly
										instr->op->arguments[a] = storeUse->op->arguments[0];
									}
								}
							}
						}
					}
				}
			}

			if(block->block->mergeFlow)
				funcops.push_back(block->block->mergeFlow);
			if(block->block->exitFlow)
			{
				// branch conditions are inlined
				if(block->block->exitFlow->flow->condition)
					erase_item(funcops, block->block->exitFlow->flow->condition);

				// return values are inlined
				if(block->block->exitFlow->opcode == spv::OpReturnValue)
				{
					SPVInstruction *arg = ids[block->block->exitFlow->flow->targets[0]];

					erase_item(funcops, arg);
				}

				funcops.push_back(block->block->exitFlow);
			}
		}

		// find redundant branch/label pairs
		for(size_t l=0; l < funcops.size()-1;)
		{
			if(funcops[l]->opcode == spv::OpBranch)
			{
				if(funcops[l+1]->opcode == spv::OpLabel && funcops[l]->flow->targets[0] == funcops[l+1]->id)
				{
					uint32_t label = funcops[l+1]->id;

					bool refd = false;

					// see if this label is a target anywhere else
					for(size_t b=0; b < funcops.size(); b++)
					{
						if(l == b) continue;

						if(funcops[b]->flow)
						{
							for(size_t t=0; t < funcops[b]->flow->targets.size(); t++)
							{
								if(funcops[b]->flow->targets[t] == label)
								{
									refd = true;
									break;
								}
							}

							if(refd)
								break;
						}
					}

					if(!refd)
					{
						funcops.erase(funcops.begin()+l);
						funcops.erase(funcops.begin()+l);
						continue;
					}
					else
					{
						// if it is refd, we can at least remove the goto
						funcops.erase(funcops.begin()+l);
						continue;
					}
				}
			}

			l++;
		}

		size_t tabSize = 2;
		size_t indent = tabSize;

		bool *varDeclared = new bool[vars.size()];
		for(size_t v=0; v < vars.size(); v++)
			varDeclared[v] = false;

		// if we're declaring variables at the top of the function rather than at first use
#if C_VARIABLE_DECLARATIONS
		for(size_t v=0; v < vars.size(); v++)
		{
			RDCASSERT(vars[v]->var && vars[v]->var->type);
			m_Disassembly += string(indent, ' ') + vars[v]->var->type->DeclareVariable(vars[v]->decorations, vars[v]->str) + ";\n";

			varDeclared[v] = true;
		}

		if(!vars.empty())
			m_Disassembly += "\n";
#endif

		vector<uint32_t> selectionstack;
		vector<uint32_t> elsestack;

		vector<uint32_t> loopheadstack;
		vector<uint32_t> loopstartstack;
		vector<uint32_t> loopmergestack;

		string funcDisassembly = "";

		for(size_t o=0; o < funcops.size(); o++)
		{
			if(funcops[o]->opcode == spv::OpLabel)
			{
				if(!elsestack.empty() && elsestack.back() == funcops[o]->id)
				{
					// handle meeting an else block
					funcDisassembly += string(indent - tabSize, ' ');
					funcDisassembly += "} else {\n";
					elsestack.pop_back();
				}
				else if(!selectionstack.empty() && selectionstack.back() == funcops[o]->id)
				{
					// handle meeting a selection merge block
					indent -= tabSize;

					funcDisassembly += string(indent, ' ');
					funcDisassembly += "}\n";
					selectionstack.pop_back();
				}
				else if(!loopmergestack.empty() && loopmergestack.back() == funcops[o]->id)
				{
					// handle meeting a loop merge block
					indent -= tabSize;

					funcDisassembly += string(indent, ' ');
					funcDisassembly += "}\n";
					loopmergestack.pop_back();
				}
				else if(!loopstartstack.empty() && loopstartstack.back() == funcops[o]->id)
				{
					// completely skip a label at the start of the loop. It's implicit from braces
				}
				else if(funcops[o]->block->mergeFlow && funcops[o]->block->mergeFlow->opcode == spv::OpLoopMerge)
				{
					// this block is a loop header
					// TODO handle if the loop header condition expression isn't sufficiently in-lined.
					// We need to force inline it.
					funcDisassembly += string(indent, ' ');
					funcDisassembly += "while(" + funcops[o]->block->exitFlow->flow->condition->Disassemble(ids, true) + ") {\n";

					loopheadstack.push_back(funcops[o]->id);
					loopstartstack.push_back(funcops[o]->block->exitFlow->flow->targets[0]);
					loopmergestack.push_back(funcops[o]->block->mergeFlow->flow->targets[0]);

					// false from the condition should jump straight to merge block
					RDCASSERT(funcops[o]->block->exitFlow->flow->targets[1] == funcops[o]->block->mergeFlow->flow->targets[0]);

					indent += tabSize;
				}
				else
				{
					funcDisassembly += funcops[o]->Disassemble(ids, false) + "\n";
				}
			}
			else if(funcops[o]->opcode == spv::OpBranch)
			{
				if(!selectionstack.empty() && funcops[o]->flow->targets[0] == selectionstack.back())
				{
					// if we're at the end of a true if path there will be a goto to
					// the merge block before the false path label. Don't output it
				}
				else if(!loopheadstack.empty() && funcops[o]->flow->targets[0] == loopheadstack.back())
				{
					if(o+1 < funcops.size() && funcops[o+1]->opcode == spv::OpLabel &&
						funcops[o+1]->id == loopmergestack.back())
					{
						// skip any gotos at the end of a loop jumping back to the header
						// block to do another loop
					}
					else
					{
						// if we're skipping to the header of the loop before the end, this is a continue
						funcDisassembly += string(indent, ' ');
						funcDisassembly += "continue;\n";
					}
				}
				else if(!loopmergestack.empty() && funcops[o]->flow->targets[0] == loopmergestack.back())
				{
					// if we're skipping to the merge of the loop without going through the
					// branch conditional, this is a break
					funcDisassembly += string(indent, ' ');
					funcDisassembly += "break;\n";
				}
				else
				{
					funcDisassembly += string(indent, ' ');
					funcDisassembly += funcops[o]->Disassemble(ids, false) + ";\n";
				}
			}
			else if(funcops[o]->opcode == spv::OpLoopMerge)
			{
				// handled above when this block started
				o++; // skip the branch conditional op
			}
			else if(funcops[o]->opcode == spv::OpSelectionMerge)
			{
				selectionstack.push_back(funcops[o]->flow->targets[0]);

				RDCASSERT(o+1 < funcops.size() && funcops[o+1]->opcode == spv::OpBranchConditional);
				o++;

				funcDisassembly += string(indent, ' ');
				funcDisassembly += "if(" + funcops[o]->Disassemble(ids, false) + ") {\n";

				indent += tabSize;

				// does the branch have an else case
				if(funcops[o]->flow->targets[1] != selectionstack.back())
					elsestack.push_back(funcops[o]->flow->targets[1]);

				RDCASSERT(o+1 < funcops.size() && funcops[o+1]->opcode == spv::OpLabel &&
					funcops[o+1]->id == funcops[o]->flow->targets[0]);
				o++; // skip outputting this label, it becomes our { essentially
			}
			else if(funcops[o]->opcode == spv::OpCompositeInsert && o+1 < funcops.size() && funcops[o+1]->opcode == spv::OpStore)
			{
				// try to merge this load-hit-store construct:
				// {id} = CompositeInsert <somevar> <foo> indices...
				// Store <somevar> {id}

				uint32_t loadID = 0;

				if(funcops[o]->op->arguments[0]->opcode == spv::OpLoad)
					loadID = funcops[o]->op->arguments[0]->op->arguments[0]->id;

				if(loadID == funcops[o+1]->op->arguments[0]->id)
				{
					// merge
					SPVInstruction *loadhit = funcops[o];
					SPVInstruction *store = funcops[o+1];

					o++;

					bool printed = false;
					
					SPVInstruction *storeVar = store->op->arguments[0];
					
					// declare variables at first use
#if !C_VARIABLE_DECLARATIONS
					for(size_t v=0; v < vars.size(); v++)
					{
						if(!varDeclared[v] && vars[v] == storeVar)
						{
							// if we're in a scope, be conservative as the variable might be
							// used after the scope - print the declaration before the scope
							// begins and continue as normal.
							if(indent > tabSize)
							{
								m_Disassembly += string(indent, ' ');
								m_Disassembly += vars[v]->var->type->DeclareVariable(vars[v]->decorations, vars[v]->str) + ";\n";
							}
							else
							{
								funcDisassembly += string(indent, ' ');
								funcDisassembly += vars[v]->var->type->DeclareVariable(vars[v]->decorations, vars[v]->str);

								printed = true;
							}

							varDeclared[v] = true;
						}
					}
#endif

					if(!printed)
					{
						string storearg;
						store->op->GetArg(ids, 0, storearg);

						funcDisassembly += string(indent, ' ');
						funcDisassembly += storearg;
					}
					funcDisassembly += loadhit->Disassemble(ids, true); // inline compositeinsert includes ' = '
					funcDisassembly += ";\n";

					loadhit->line = (int)o;
				}
				else
				{
					// print separately
					funcDisassembly += string(indent, ' ');
					funcDisassembly += funcops[o]->Disassemble(ids, false) + ";\n";
					funcops[o]->line = (int)o;

					o++;
					
					SPVInstruction *storeVar = funcops[o]->op->arguments[0];

					bool printed = false;

					// declare variables at first use
#if !C_VARIABLE_DECLARATIONS
					for(size_t v=0; v < vars.size(); v++)
					{
						if(!varDeclared[v] && vars[v] == storeVar)
						{
							// if we're in a scope, be conservative as the variable might be
							// used after the scope - print the declaration before the scope
							// begins and continue as normal.
							if(indent > tabSize)
							{
								m_Disassembly += string(indent, ' ');
								m_Disassembly += vars[v]->var->type->DeclareVariable(vars[v]->decorations, vars[v]->str) + ";\n";
							}
							else
							{
								funcDisassembly += string(indent, ' ');
								funcDisassembly += vars[v]->var->type->DeclareVariable(vars[v]->decorations, vars[v]->str) + " = ";
								funcDisassembly += funcops[o]->Disassemble(ids, true) + ";\n";

								printed = true;
							}

							varDeclared[v] = true;
						}
					}
#endif
					
					if(!printed)
					{
						funcDisassembly += string(indent, ' ');
						funcDisassembly += funcops[o]->Disassemble(ids, false) + ";\n";
					}
				}
			}
			else if(funcops[o]->opcode == spv::OpReturn && o == funcops.size()-1)
			{
				// don't print the return statement if it's the last statement in a function
				break;
			}
			else if(funcops[o]->opcode == spv::OpStore)
			{
				SPVInstruction *storeVar = funcops[o]->op->arguments[0];

				bool printed = false;

				// declare variables at first use
#if !C_VARIABLE_DECLARATIONS
				for(size_t v=0; v < vars.size(); v++)
				{
					if(!varDeclared[v] && vars[v] == storeVar)
					{
						// if we're in a scope, be conservative as the variable might be
						// used after the scope - print the declaration before the scope
						// begins and continue as normal.
						if(indent > tabSize)
						{
							m_Disassembly += string(indent, ' ');
							m_Disassembly += vars[v]->var->type->DeclareVariable(vars[v]->decorations, vars[v]->str) + ";\n";
						}
						else
						{
							funcDisassembly += string(indent, ' ');
							funcDisassembly += vars[v]->var->type->DeclareVariable(vars[v]->decorations, vars[v]->str) + " = ";
							funcDisassembly += funcops[o]->Disassemble(ids, true) + ";\n";

							printed = true;
						}

						varDeclared[v] = true;
					}
				}
#endif

				if(!printed)
				{
					funcDisassembly += string(indent, ' ');
					funcDisassembly += funcops[o]->Disassemble(ids, false) + ";\n";
				}
			}
			else
			{
				funcDisassembly += string(indent, ' ');
				funcDisassembly += funcops[o]->Disassemble(ids, false) + ";\n";
			}

			funcops[o]->line = (int)o;
		}

		m_Disassembly += funcDisassembly;

		SAFE_DELETE_ARRAY(varDeclared);

		m_Disassembly += StringFormat::Fmt("} // %s\n\n", funcs[f]->str.c_str());
	}
}

void MakeConstantBlockVariables(SPVTypeData *type, rdctype::array<ShaderConstant> &cblock)
{
	RDCASSERT(!type->children.empty());

	create_array_uninit(cblock, type->children.size());
	for(size_t i=0; i < type->children.size(); i++)
	{
		SPVTypeData *t = type->children[i].first;
		cblock[i].name = type->children[i].second;
		// TODO do we need to fill these out?
		cblock[i].reg.vec = 0;
		cblock[i].reg.comp = 0;

		string suffix = "";

		cblock[i].type.descriptor.elements = 1;

		if(t->type == SPVTypeData::eArray)
		{
			suffix += StringFormat::Fmt("[%u]", t->arraySize);
			cblock[i].type.descriptor.elements = t->arraySize;
			t = t->baseType;
		}

		if(t->type == SPVTypeData::eVector ||
			 t->type == SPVTypeData::eMatrix)
		{
			if(t->baseType->type == SPVTypeData::eFloat)
				cblock[i].type.descriptor.type = eVar_Float;
			else if(t->baseType->type == SPVTypeData::eUInt)
				cblock[i].type.descriptor.type = eVar_UInt;
			else if(t->baseType->type == SPVTypeData::eSInt)
				cblock[i].type.descriptor.type = eVar_Int;
			else
				RDCERR("Unexpected base type of constant variable %u", t->baseType->type);

			cblock[i].type.descriptor.rowMajorStorage = false;
			
			for(size_t d=0; d < type->decorations[i].size(); d++)
				if(type->decorations[i][d].decoration == spv::DecorationRowMajor)
					cblock[i].type.descriptor.rowMajorStorage = true;

			if(t->type == SPVTypeData::eMatrix)
			{
				cblock[i].type.descriptor.rows = t->vectorSize;
				cblock[i].type.descriptor.cols = t->matrixSize;
			}
			else
			{
				cblock[i].type.descriptor.rows = 1;
				cblock[i].type.descriptor.cols = t->vectorSize;
			}

			cblock[i].type.descriptor.name = t->GetName() + suffix;
		}
		else if(t->IsScalar())
		{
			if(t->type == SPVTypeData::eFloat)
				cblock[i].type.descriptor.type = eVar_Float;
			else if(t->type == SPVTypeData::eUInt)
				cblock[i].type.descriptor.type = eVar_UInt;
			else if(t->type == SPVTypeData::eSInt)
				cblock[i].type.descriptor.type = eVar_Int;
			else
				RDCERR("Unexpected base type of constant variable %u", t->type);

			cblock[i].type.descriptor.rowMajorStorage = false;
			cblock[i].type.descriptor.rows = 1;
			cblock[i].type.descriptor.cols = 1;

			cblock[i].type.descriptor.name = t->GetName() + suffix;
		}
		else
		{
			cblock[i].type.descriptor.type = eVar_Float;
			cblock[i].type.descriptor.rowMajorStorage = false;
			cblock[i].type.descriptor.rows = 0;
			cblock[i].type.descriptor.cols = 0;

			cblock[i].type.descriptor.name = t->GetName() + suffix;

			MakeConstantBlockVariables(t, cblock[i].type.members);
		}
	}
}

SystemAttribute BuiltInToSystemAttribute(const spv::BuiltIn el)
{
	// not complete, might need to expand system attribute list

	switch(el)
	{
		case spv::BuiltInPosition:                         return eAttr_Position;
		case spv::BuiltInPointSize:                        return eAttr_PointSize;
		case spv::BuiltInClipDistance:                     return eAttr_ClipDistance;
		case spv::BuiltInCullDistance:                     return eAttr_CullDistance;
		case spv::BuiltInVertexId:                         return eAttr_VertexIndex;
		case spv::BuiltInInstanceId:                       return eAttr_InstanceIndex;
		case spv::BuiltInPrimitiveId:                      return eAttr_PrimitiveIndex;
		case spv::BuiltInInvocationId:                     return eAttr_InvocationIndex;
		case spv::BuiltInLayer:                            return eAttr_RTIndex;
		case spv::BuiltInViewportIndex:                    return eAttr_ViewportIndex;
		case spv::BuiltInTessLevelOuter:                   return eAttr_OuterTessFactor;
		case spv::BuiltInTessLevelInner:                   return eAttr_InsideTessFactor;
		case spv::BuiltInPatchVertices:                    return eAttr_PatchNumVertices;
		case spv::BuiltInFrontFacing:                      return eAttr_IsFrontFace;
		case spv::BuiltInSampleId:                         return eAttr_MSAASampleIndex;
		case spv::BuiltInSamplePosition:                   return eAttr_MSAASamplePosition;
		case spv::BuiltInSampleMask:                       return eAttr_MSAACoverage;
		case spv::BuiltInFragColor:                        return eAttr_ColourOutput;
		case spv::BuiltInFragDepth:                        return eAttr_DepthOutput;
		//case spv::BuiltInVertexIndex:                      return eAttr_Vertex0Index;
		//case spv::BuiltInInstanceIndex:                    return eAttr_Instance0Index;
		default: break;
	}
	
	return eAttr_None;
}

template<typename T>
struct bindpair
{
	BindpointMap map;
	T bindres;

	bindpair(const BindpointMap &m, const T &res)
		: map(m), bindres(res)
	{}

	bool operator <(const bindpair &o) const
	{
		if(map.bindset != o.map.bindset)
			return map.bindset < o.map.bindset;

		// sort -1 to the end
		if(map.bind == -1 && o.map.bind == -1) // equal
			return false;
		if(map.bind == -1) // -1 not less than anything
			return false;
		if(o.map.bind == -1) // anything less than -1
			return true;

		return map.bind < o.map.bind;
	}
};

typedef bindpair<ConstantBlock> cblockpair;
typedef bindpair<ShaderResource> shaderrespair;

void AddSignatureParameter(uint32_t id, uint32_t childIdx, string varName, SPVTypeData *type, const vector<SPVDecoration> &decorations, vector<SigParameter> &sigarray, rdctype::array<int> *inputAttrs)
{
	SigParameter sig;

	sig.varName = varName;
	sig.needSemanticIndex = false;

	// this is super cheeky, but useful to pick up when doing output dumping and
	// these properties won't be used elsewhere. We should really share the data
	// in a better way though.
	sig.semanticIdxName = StringFormat::Fmt("%u", id);
	sig.semanticIndex = childIdx;

	bool rowmajor = true;

	sig.regIndex = 0;
	for(size_t d=0; d < decorations.size(); d++)
	{
		if(decorations[d].decoration == spv::DecorationLocation)
			sig.regIndex = decorations[d].val;
		else if(decorations[d].decoration == spv::DecorationBuiltIn)
			sig.systemValue = BuiltInToSystemAttribute((spv::BuiltIn)decorations[d].val);
		else if(decorations[d].decoration == spv::DecorationRowMajor)
			rowmajor = true;
		else if(decorations[d].decoration == spv::DecorationColMajor)
			rowmajor = false;
	}

	RDCASSERT(sig.regIndex < 16);

	if(type->type == SPVTypeData::ePointer)
		type = type->baseType;

	if(type->type == SPVTypeData::eStruct)
	{
		// we don't support nested structs yet
		RDCASSERT(childIdx == ~0U);
		for(size_t c=0; c < type->children.size(); c++)
			AddSignatureParameter(id, (uint32_t)c, varName + "." + type->children[c].second, type->children[c].first, type->decorations[c], sigarray, inputAttrs);
		return;
	}

	switch(type->baseType ? type->baseType->type : type->type)
	{
		case SPVTypeData::eBool:
		case SPVTypeData::eUInt:
			sig.compType = eCompType_UInt;
			break;
		case SPVTypeData::eSInt:
			sig.compType = eCompType_SInt;
			break;
		case SPVTypeData::eFloat:
			sig.compType = eCompType_Float;
			break;
		default:
			RDCERR("Unexpected base type of input/output signature %u", type->baseType ? type->baseType->type : type->type);
			break;
	}

	sig.compCount = type->vectorSize;
	sig.stream = 0;

	sig.regChannelMask = sig.channelUsedMask = (1<<type->vectorSize)-1;

	if(type->matrixSize == 1)
	{
		if(inputAttrs && sig.systemValue == eAttr_None)
			inputAttrs->elems[sig.regIndex] = (int32_t)sigarray.size();
		sigarray.push_back(sig);
	}
	else
	{
		for(uint32_t m=0; m < type->matrixSize; m++)
		{
			SigParameter s = sig;
			s.varName = StringFormat::Fmt("%s:%s%u", varName.c_str(), rowmajor ? "row" : "col", m);
			s.regIndex += m;

			RDCASSERT(s.regIndex < 16);

			if(inputAttrs && sig.systemValue == eAttr_None)
				inputAttrs->elems[s.regIndex] = (int32_t)sigarray.size();

			sigarray.push_back(s);
		}
	}
}

void SPVModule::MakeReflection(ShaderReflection *reflection, ShaderBindpointMapping *mapping)
{
	vector<SigParameter> inputs;
	vector<SigParameter> outputs;
	vector<cblockpair> cblocks;
	vector<shaderrespair> resources;

	create_array_uninit(mapping->InputAttributes, 16);
	for(size_t i=0; i < 16; i++) mapping->InputAttributes[i] = -1;

	// TODO need to fetch these
	reflection->DispatchThreadsDimension[0] = 0;
	reflection->DispatchThreadsDimension[1] = 0;
	reflection->DispatchThreadsDimension[2] = 0;

	for(size_t i=0; i < globals.size(); i++)
	{
		SPVInstruction *inst = globals[i];
		if(inst->var->storage == spv::StorageClassInput || inst->var->storage == spv::StorageClassOutput)
		{
			bool isInput = inst->var->storage == spv::StorageClassInput;
			vector<SigParameter> *sigarray = (isInput ? &inputs : &outputs);

			string nm;
			// try to use the instance/variable name
			if(!inst->str.empty())
				nm = inst->str;
			// for structs, if there's no instance name, use the type name
			else if(inst->var->type->type == SPVTypeData::ePointer && inst->var->type->baseType->type == SPVTypeData::eStruct)
				nm = inst->var->type->baseType->name;
			// otherwise fall back to naming after the ID
			else
				nm = StringFormat::Fmt("sig%u", inst->id);

			AddSignatureParameter(inst->id, ~0U, nm, inst->var->type, inst->decorations, *sigarray, isInput ? &mapping->InputAttributes : NULL);
		}
		else if(inst->var->storage == spv::StorageClassUniform ||
		        inst->var->storage == spv::StorageClassUniformConstant ||
		        inst->var->storage == spv::StorageClassPushConstant)
		{
			bool pushConst = (inst->var->storage == spv::StorageClassPushConstant);

			SPVTypeData *type = inst->var->type;
			if(type->type == SPVTypeData::ePointer)
				type = type->baseType;

			uint32_t arraySize = 1;
			if(type->type == SPVTypeData::eArray)
			{
				arraySize = type->arraySize;
				type = type->baseType;
			}

			if(type->type == SPVTypeData::eStruct)
			{
				ConstantBlock cblock;

				if(!inst->str.empty())
					cblock.name = inst->str;
				else if(!type->name.empty())
					cblock.name = type->name;
				else
					cblock.name = StringFormat::Fmt("uniforms%u", inst->id);
				cblock.bufferBacked = !pushConst;
				
				BindpointMap bindmap = {0};
				// set can be implicitly 0, but the binding must be set explicitly.
				// If no binding is found, we set -1 and sort to the end of the resources
				// list as it's not bound anywhere (most likely, declared but not used)
				bindmap.bind = -1;

				for(size_t d=0; d < inst->decorations.size(); d++)
				{
					if(inst->decorations[d].decoration == spv::DecorationDescriptorSet)
						bindmap.bindset = (int32_t)inst->decorations[d].val;
					if(inst->decorations[d].decoration == spv::DecorationBinding)
						bindmap.bind = (int32_t)inst->decorations[d].val;
				}

				MakeConstantBlockVariables(type, cblock.variables);


				bindmap.used = false;

				bindmap.arraySize = arraySize;

				for(size_t o=0; o < operations.size(); o++)
				{
					if(operations[o]->op)
					{
						for(size_t a=0; a < operations[o]->op->arguments.size(); a++)
						{
							if(operations[o]->op->arguments[a] == inst)
							{
								bindmap.used = true;
								break;
							}
						}
					}
				}

				// should never have elements that have no binding declared but
				// are used, unless it's push constants (which is handled elsewhere)
				RDCASSERT(!bindmap.used || !cblock.bufferBacked || bindmap.bind >= 0);
				
				cblocks.push_back(cblockpair(bindmap, cblock));
			}
			else
			{
				ShaderResource res;

				res.name = inst->str.empty() ? StringFormat::Fmt("res%u", inst->id) : inst->str;

				if(type->multisampled)
					res.resType = type->arrayed ? eResType_Texture2DMSArray : eResType_Texture2DMS;
				else if(type->texdim == spv::Dim1D)
					res.resType = type->arrayed ? eResType_Texture1DArray : eResType_Texture1D;
				else if(type->texdim == spv::Dim2D)
					res.resType = type->arrayed ? eResType_Texture2DArray : eResType_Texture2D;
				else if(type->texdim == spv::DimCube)
					res.resType = type->arrayed ? eResType_TextureCubeArray : eResType_TextureCube;
				else if(type->texdim == spv::Dim3D)
					res.resType = eResType_Texture3D;
				else if(type->texdim == spv::DimRect)
					res.resType = eResType_TextureRect;
				else if(type->texdim == spv::DimBuffer)
					res.resType = eResType_Buffer;

				// TODO once we're on SPIR-V 1.0, update this handling
				res.IsSampler = true;
				res.IsTexture = true;
				res.IsSRV = true;

				SPVTypeData *sampledType = type->baseType;
				if(sampledType->type == SPVTypeData::eImage)
					sampledType = sampledType->baseType;

				if(sampledType->type == SPVTypeData::eFloat)
					res.variableType.descriptor.type = eVar_Float;
				else if(sampledType->type == SPVTypeData::eUInt)
					res.variableType.descriptor.type = eVar_UInt;
				else if(sampledType->type == SPVTypeData::eSInt)
					res.variableType.descriptor.type = eVar_Int;
				else
					RDCERR("Unexpected base type of resource %u", sampledType->type);

				res.variableType.descriptor.rows = 1;
				res.variableType.descriptor.cols = 1;
				res.variableType.descriptor.elements = 1;
				res.variableType.descriptor.rowMajorStorage = false;
				res.variableType.descriptor.rowMajorStorage = false;

				BindpointMap bindmap = {0};
				// set can be implicitly 0, but the binding must be set explicitly.
				// If no binding is found, we set -1 and sort to the end of the resources
				// list as it's not bound anywhere (most likely, declared but not used)
				bindmap.bind = -1;

				for(size_t d=0; d < inst->decorations.size(); d++)
				{
					if(inst->decorations[d].decoration == spv::DecorationDescriptorSet)
						bindmap.bindset = (int32_t)inst->decorations[d].val;
					if(inst->decorations[d].decoration == spv::DecorationBinding)
						bindmap.bind = (int32_t)inst->decorations[d].val;
				}

				bindmap.used = false;

				bindmap.arraySize = arraySize;

				for(size_t o=0; o < operations.size(); o++)
				{
					if(operations[o]->op)
					{
						for(size_t a=0; a < operations[o]->op->arguments.size(); a++)
						{
							if(operations[o]->op->arguments[a] == inst)
							{
								bindmap.used = true;
								break;
							}
						}
					}
				}

				// should never have elements that have no binding declared but
				// are used
				RDCASSERT(!bindmap.used || bindmap.bind >= 0);

				resources.push_back(shaderrespair(bindmap, res));
			}
		}
		else
		{
			RDCWARN("Unexpected storage class for global: %s", ToStr::Get(inst->var->storage).c_str());
		}
	}
	
	// sort system value semantics to the start of the list
	struct sig_param_sort
	{
		bool operator() (const SigParameter &a, const SigParameter &b)
		{
			if(a.systemValue == b.systemValue) return a.regIndex < b.regIndex;
			if(a.systemValue == eAttr_None)
				return false;
			if(b.systemValue == eAttr_None)
				return true;
			
			return a.systemValue < b.systemValue;
		}
	};

	std::sort(inputs.begin(), inputs.end(), sig_param_sort());
	std::sort(outputs.begin(), outputs.end(), sig_param_sort());

	reflection->InputSig = inputs;
	reflection->OutputSig = outputs;

	std::sort(cblocks.begin(), cblocks.end());
	std::sort(resources.begin(), resources.end());

	create_array_uninit(mapping->ConstantBlocks, cblocks.size());
	create_array_uninit(reflection->ConstantBlocks, cblocks.size());

	create_array_uninit(mapping->ReadOnlyResources, resources.size());
	create_array_uninit(reflection->ReadOnlyResources, resources.size());

	for(size_t i=0; i < cblocks.size(); i++)
	{
		mapping->ConstantBlocks[i] = cblocks[i].map;
		// fix up any bind points marked with -1. They were sorted to the end
		// but from here on we want to just be able to index with the bind point
		// without any special casing.
		if(mapping->ConstantBlocks[i].bind == -1)
			mapping->ConstantBlocks[i].bind = 0;
		reflection->ConstantBlocks[i] = cblocks[i].bindres;
		reflection->ConstantBlocks[i].bindPoint = (int32_t)i;
	}

	for(size_t i=0; i < resources.size(); i++)
	{
		mapping->ReadOnlyResources[i] = resources[i].map;
		// fix up any bind points marked with -1. They were sorted to the end
		// but from here on we want to just be able to index with the bind point
		// without any special casing.
		if(mapping->ReadOnlyResources[i].bind == -1)
			mapping->ReadOnlyResources[i].bind = 0;
		reflection->ReadOnlyResources[i] = resources[i].bindres;
		reflection->ReadOnlyResources[i].bindPoint = (int32_t)i;
	}
}

void ParseSPIRV(uint32_t *spirv, size_t spirvLength, SPVModule &module)
{
	if(spirv[0] != (uint32_t)spv::MagicNumber)
	{
		RDCERR("Unrecognised SPIR-V magic number %08x", spirv[0]);
		return;
	}
	
	module.moduleVersion = spirv[1];

	if(module.moduleVersion != spv::Version)
	{
		RDCERR("Unsupported SPIR-V version: %08x", spirv[1]);
		return;
	}

	module.spirv.assign(spirv, spirv+spirvLength);

	module.generator = spirv[2];
	module.ids.resize(spirv[3]);

	uint32_t idbound = spirv[3];

	RDCASSERT(spirv[4] == 0);

	SPVFunction *curFunc = NULL;
	SPVBlock *curBlock = NULL;
	
	size_t it = 5;
	while(it < spirvLength)
	{
		uint16_t WordCount = spirv[it]>>spv::WordCountShift;

		module.operations.push_back(new SPVInstruction());
		SPVInstruction &op = *module.operations.back();

		op.opcode = spv::Op(spirv[it]&spv::OpCodeMask);

		bool mathop = false;

		switch(op.opcode)
		{
			//////////////////////////////////////////////////////////////////////
			// 'Global' opcodes
			case spv::OpSource:
			{
				module.sourceLang = spv::SourceLanguage(spirv[it+1]);
				module.sourceVer = spirv[it+2];

				if(WordCount > 3)
				{
					RDCDEBUG("Filename provided");
					// VKTODOLOW spirv[it+3] is an id of an OpString with a filename
				}

				if(WordCount > 4)
				{
					RDCDEBUG("File source provided");
					// VKTODOLOW spirv[it+4] is a literal string with source of the file
				}

				break;
			}
			case spv::OpSourceContinued:
			{
				RDCDEBUG("File source continued");
				// VKTODOLOW spirv[it+1] is a literal string to append to the last OpSource
				break;
			}
			case spv::OpSourceExtension:
			{
				op.str = (const char *)&spirv[it+1];
				module.sourceexts.push_back(&op);
				break;
			}
			case spv::OpCapability:
			{
				module.capabilities.push_back((spv::Capability)spirv[it+1]);
				break;
			}
			case spv::OpMemoryModel:
			{
				// do we care about this?
				spv::AddressingModel addr = spv::AddressingModel(spirv[it+1]);
				spv::MemoryModel mem = spv::MemoryModel(spirv[it+2]);
				break;
			}
			case spv::OpEntryPoint:
			{
				op.entry = new SPVEntryPoint();
				op.entry->func = spirv[it+2];
				op.entry->model = spv::ExecutionModel(spirv[it+1]);
				op.entry->name = (const char *)&spirv[it+3];

				// VKTODOLOW look at interface IDs?
				module.entries.push_back(&op);
				break;
			}
			case spv::OpExecutionMode:
			{
				uint32_t func = spirv[it+1];
				for(size_t e=0; e < module.entries.size(); e++)
				{
					if(module.entries[e]->entry->func == func)
					{
						SPVExecutionMode mode;
						mode.mode = (spv::ExecutionMode)spirv[it+2];
						
						if(WordCount > 3) mode.x = spirv[it+3];
						if(WordCount > 4) mode.y = spirv[it+4];
						if(WordCount > 5) mode.z = spirv[it+5];

						module.entries[e]->entry->modes.push_back(mode);
						break;
					}
				}
				break;
			}
			case spv::OpExtInstImport:
			{
				op.ext = new SPVExtInstSet();
				op.ext->setname = (const char *)&spirv[it+2];
				op.ext->instructions = NULL;

				if(op.ext->setname == "GLSL.std.450")
				{
					op.ext->instructions = GLSL_STD_450_names;

					if(GLSL_STD_450_names[0] == NULL)
						GLSL_STD_450::GetDebugNames(GLSL_STD_450_names);
				}

				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpString:
			{
				op.str = (const char *)&spirv[it+2];

				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Type opcodes
			case spv::OpTypeVoid:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eVoid;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeBool:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eBool;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeInt:
			{
				op.type = new SPVTypeData();
				op.type->type = spirv[it+3] ? SPVTypeData::eSInt : SPVTypeData::eUInt;
				op.type->bitCount = spirv[it+2];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeFloat:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eFloat;
				op.type->bitCount = spirv[it+2];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeVector:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eVector;

				SPVInstruction *baseTypeInst = module.GetByID(spirv[it+2]);
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;
				op.type->vectorSize = spirv[it+3];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeMatrix:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eMatrix;

				SPVInstruction *baseTypeInst = module.GetByID(spirv[it+2]);
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				RDCASSERT(baseTypeInst->type->type == SPVTypeData::eVector);

				op.type->baseType = baseTypeInst->type->baseType;
				op.type->vectorSize = baseTypeInst->type->vectorSize;
				op.type->matrixSize = spirv[it+3];
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeArray:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eArray;

				SPVInstruction *baseTypeInst = module.GetByID(spirv[it+2]);
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;

				SPVInstruction *sizeInst = module.GetByID(spirv[it+3]);
				RDCASSERT(sizeInst && sizeInst->constant && sizeInst->constant->type->IsBasicInt());

				op.type->arraySize = sizeInst->constant->u32;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeStruct:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eStruct;

				for(int i=2; i < WordCount; i++)
				{
					SPVInstruction *memberInst = module.GetByID(spirv[it+i]);
					RDCASSERT(memberInst && memberInst->type);

					// names might come later from OpMemberName instructions
					op.type->children.push_back(make_pair(memberInst->type, ""));
					op.type->decorations.push_back(vector<SPVDecoration>());
				}

				module.structs.push_back(&op);
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypePointer:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::ePointer;

				SPVInstruction *baseTypeInst = module.GetByID(spirv[it+3]);
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;
				op.type->storage = spv::StorageClass(spirv[it+2]);
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeImage:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eImage;

				SPVInstruction *baseTypeInst = module.GetByID(spirv[it+2]);
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;

				op.type->texdim = spv::Dim(spirv[it+3]);
				op.type->depth = spirv[it+4] != 0;
				op.type->arrayed = spirv[it+5] != 0;
				op.type->multisampled = spirv[it+6] != 0;
				op.type->sampled = spirv[it+7];
				op.type->imgformat = spv::ImageFormat(spirv[it+8]);

				// not checking access qualifier
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeSampler:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eSampler;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeSampledImage:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eSampledImage;

				SPVInstruction *baseTypeInst = module.GetByID(spirv[it+2]);
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				op.type->baseType = baseTypeInst->type;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpTypeFunction:
			{
				op.type = new SPVTypeData();
				op.type->type = SPVTypeData::eFunction;

				for(int i=3; i < WordCount; i++)
				{
					SPVInstruction *argInst = module.GetByID(spirv[it+i]);
					RDCASSERT(argInst && argInst->type);

					// function parameters have no name
					op.type->children.push_back(make_pair(argInst->type, ""));
					op.type->decorations.push_back(vector<SPVDecoration>());
				}

				SPVInstruction *baseTypeInst = module.GetByID(spirv[it+2]);
				RDCASSERT(baseTypeInst && baseTypeInst->type);

				// return type
				op.type->baseType = baseTypeInst->type;
				
				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Constants
			case spv::OpConstantTrue:
			case spv::OpConstantFalse:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.constant = new SPVConstant();
				op.constant->type = typeInst->type;

				op.constant->u32 = op.opcode == spv::OpConstantTrue ? 1 : 0;

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				break;
			}
			case spv::OpConstant:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.constant = new SPVConstant();
				op.constant->type = typeInst->type;

				op.constant->u32 = spirv[it+3];

				if(WordCount > 3)
				{
					// only handle 32-bit or 64-bit constants
					RDCASSERT(WordCount <= 4);

					uint64_t lo = spirv[it+3];
					uint64_t hi = spirv[it+4];

					op.constant->u64 = lo | (hi<<32);
				}

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				break;
			}
			case spv::OpConstantComposite:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.constant = new SPVConstant();
				op.constant->type = typeInst->type;

				for(int i=3; i < WordCount; i++)
				{
					SPVInstruction *constInst = module.GetByID(spirv[it+i]);
					RDCASSERT(constInst && constInst->constant);

					op.constant->children.push_back(constInst->constant);
				}

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				break;
			}
			case spv::OpConstantSampler:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.constant = new SPVConstant();
				op.constant->type = typeInst->type;

				op.constant->sampler.addressing = spv::SamplerAddressingMode(spirv[it+3]);
				op.constant->sampler.normalised = (spirv[it+4] != 0);
				op.constant->sampler.filter = spv::SamplerFilterMode(spirv[it+5]);

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Functions
			case spv::OpFunction:
			{
				SPVInstruction *retTypeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(retTypeInst && retTypeInst->type);

				SPVInstruction *typeInst = module.GetByID(spirv[it+4]);
				RDCASSERT(typeInst && typeInst->type);

				op.func = new SPVFunction();
				op.func->retType = retTypeInst->type;
				op.func->funcType = typeInst->type;
				op.func->control = spv::FunctionControlMask(spirv[it+3]);

				module.funcs.push_back(&op);

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				curFunc = op.func;

				break;
			}
			case spv::OpFunctionEnd:
			{
				curFunc = NULL;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Variables
			case spv::OpVariable:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.var = new SPVVariable();
				op.var->type = typeInst->type;
				op.var->storage = spv::StorageClass(spirv[it+3]);

				if(WordCount > 4)
				{
					SPVInstruction *initInst = module.GetByID(spirv[it+4]);
					RDCASSERT(initInst && initInst->constant);
					op.var->initialiser = initInst->constant;
				}

				if(curFunc) curFunc->variables.push_back(&op);
				else module.globals.push_back(&op);

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				break;
			}
			case spv::OpFunctionParameter:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.var = new SPVVariable();
				op.var->type = typeInst->type;
				op.var->storage = spv::StorageClassFunction;

				RDCASSERT(curFunc);
				curFunc->arguments.push_back(&op);

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Branching/flow control
			case spv::OpLabel:
			{
				op.block = new SPVBlock();

				RDCASSERT(curFunc);

				curFunc->blocks.push_back(&op);
				curBlock = op.block;

				op.id = spirv[it+1];
				module.ids[spirv[it+1]] = &op;
				break;
			}
			case spv::OpKill:
			case spv::OpUnreachable:
			case spv::OpReturn:
			{
				op.flow = new SPVFlowControl();

				curBlock->exitFlow = &op;
				curBlock = NULL;
				break;
			}
			case spv::OpReturnValue:
			{
				op.flow = new SPVFlowControl();

				op.flow->targets.push_back(spirv[it+1]);

				curBlock->exitFlow = &op;
				curBlock = NULL;
				break;
			}
			case spv::OpBranch:
			{
				op.flow = new SPVFlowControl();

				op.flow->targets.push_back(spirv[it+1]);

				curBlock->exitFlow = &op;
				curBlock = NULL;
				break;
			}
			case spv::OpBranchConditional:
			{
				op.flow = new SPVFlowControl();

				SPVInstruction *condInst = module.GetByID(spirv[it+1]);
				RDCASSERT(condInst);

				op.flow->condition = condInst;
				op.flow->targets.push_back(spirv[it+2]);
				op.flow->targets.push_back(spirv[it+3]);

				if(WordCount == 6)
				{
					op.flow->literals.push_back(spirv[it+4]);
					op.flow->literals.push_back(spirv[it+5]);
				}

				curBlock->exitFlow = &op;
				curBlock = NULL;
				break;
			}
			case spv::OpSelectionMerge:
			{
				op.flow = new SPVFlowControl();

				op.flow->targets.push_back(spirv[it+1]);
				op.flow->selControl = spv::SelectionControlMask(spirv[it+2]);

				curBlock->mergeFlow = &op;
				break;
			}
			case spv::OpLoopMerge:
			{
				op.flow = new SPVFlowControl();

				op.flow->targets.push_back(spirv[it+1]);
				op.flow->loopControl = spv::LoopControlMask(spirv[it+2]);

				curBlock->mergeFlow = &op;
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Operations with special parameters
			case spv::OpLoad:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;
				
				SPVInstruction *ptrInst = module.GetByID(spirv[it+3]);
				RDCASSERT(ptrInst);

				op.op->arguments.push_back(ptrInst);

				op.op->access = spv::MemoryAccessMaskNone;
				if(WordCount > 4)
						op.op->access = spv::MemoryAccessMask(spirv[it+4]);
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				
				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpStore:
			case spv::OpCopyMemory:
			{
				op.op = new SPVOperation();
				op.op->type = NULL;
				
				SPVInstruction *ptrInst = module.GetByID(spirv[it+1]);
				RDCASSERT(ptrInst);
				
				SPVInstruction *valInst = module.GetByID(spirv[it+2]);
				RDCASSERT(valInst);

				op.op->arguments.push_back(ptrInst);
				op.op->arguments.push_back(valInst);

				op.op->access = spv::MemoryAccessMaskNone;
				if(WordCount > 3)
						op.op->access = spv::MemoryAccessMask(spirv[it+4]);
				
				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpImageSampleImplicitLod:
			case spv::OpImageSampleExplicitLod:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);
				
				op.op = new SPVOperation();
				op.op->type = typeInst->type;

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				// sampled image
				{
					SPVInstruction *argInst = module.GetByID(spirv[it+3]);
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}
				
				// co-ords
				{
					SPVInstruction *argInst = module.GetByID(spirv[it+4]);
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}

				// const argument bitfield

				// optional arguments
				{
				}
				
				curBlock->instructions.push_back(&op);
				break;
			}
			// conversions can be treated as if they were function calls
			case spv::OpConvertFToS:
			case spv::OpConvertFToU:
			case spv::OpConvertUToF:
			case spv::OpConvertSToF:
			case spv::OpBitcast:
			case spv::OpFunctionCall:
			{
				int word = 1;

				SPVInstruction *typeInst = module.GetByID(spirv[it+word]);
				RDCASSERT(typeInst && typeInst->type);

				word++;

				op.op = new SPVOperation();
				op.op->type = typeInst->type;

				op.id = spirv[it+word];
				module.ids[spirv[it+word]] = &op;

				word++;

				if(op.opcode == spv::OpFunctionCall)
				{
					op.op->funcCall = spirv[it+word];

					word++;
				}

				for(; word < WordCount; word++)
				{
					SPVInstruction *argInst = module.GetByID(spirv[it+word]);
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}

				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpVectorShuffle:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;

				{
					SPVInstruction *argInst = module.GetByID(spirv[it+3]);
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}

				{
					SPVInstruction *argInst = module.GetByID(spirv[it+4]);
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}

				for(int i=5; i < WordCount; i++)
					op.op->literals.push_back(spirv[it+i]);

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpExtInst:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;

				{
					SPVInstruction *setInst = module.GetByID(spirv[it+3]);
					RDCASSERT(setInst);

					op.op->arguments.push_back(setInst);
				}

				op.op->literals.push_back(spirv[it+4]);

				for(int i=5; i < WordCount; i++)
				{
					SPVInstruction *argInst = module.GetByID(spirv[it+i]);
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}

				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;

				curBlock->instructions.push_back(&op);
				break;
			}
			//////////////////////////////////////////////////////////////////////
			// Easy to handle opcodes with just some number of ID parameters
			case spv::OpIAdd:
			case spv::OpFAdd:
			case spv::OpISub:
			case spv::OpFSub:
			case spv::OpIMul:
			case spv::OpFMul:
			case spv::OpFDiv:
			case spv::OpFMod:
			case spv::OpVectorTimesScalar:
			case spv::OpMatrixTimesVector:
			case spv::OpMatrixTimesMatrix:
			case spv::OpSLessThan:
			case spv::OpSLessThanEqual:
			case spv::OpFOrdLessThan:
			case spv::OpFOrdGreaterThan:
			case spv::OpFOrdGreaterThanEqual:
			case spv::OpLogicalAnd:
			case spv::OpLogicalOr:
			case spv::OpLogicalNotEqual:
			case spv::OpShiftLeftLogical:

			case spv::OpFNegate:
			case spv::OpNot:
			case spv::OpLogicalNot:
				mathop = true; // deliberate fallthrough
				
			case spv::OpCompositeConstruct:
			case spv::OpAccessChain:
			case spv::OpDot:
			case spv::OpSelect:
			{
				SPVInstruction *typeInst = module.GetByID(spirv[it+1]);
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;
				op.op->mathop = mathop;
				
				for(int i=3; i < WordCount; i++)
				{
					SPVInstruction *argInst = module.GetByID(spirv[it+i]);
					RDCASSERT(argInst);

					op.op->arguments.push_back(argInst);
				}
				
				op.id = spirv[it+2];
				module.ids[spirv[it+2]] = &op;
				
				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpCompositeExtract:
			case spv::OpCompositeInsert:
			{
				int word = 1;

				SPVInstruction *typeInst = module.GetByID(spirv[it+word]);
				RDCASSERT(typeInst && typeInst->type);

				op.op = new SPVOperation();
				op.op->type = typeInst->type;

				word++;
				
				op.id = spirv[it+word];
				module.ids[spirv[it+word]] = &op;

				word++;

				SPVInstruction *objInst = NULL;
				if(op.opcode == spv::OpCompositeInsert)
				{
					op.op->complexity = 100; // never combine composite insert

					objInst = module.GetByID(spirv[it+word]);
					RDCASSERT(objInst);

					word++;
				}
				
				SPVInstruction *compInst = module.GetByID(spirv[it+word]);
				RDCASSERT(compInst);
				
				word++;

				op.op->arguments.push_back(compInst);
				if(objInst)
					op.op->arguments.push_back(objInst);
				
				for(; word < WordCount; word++)
					op.op->literals.push_back(spirv[it+word]);

				curBlock->instructions.push_back(&op);
				break;
			}
			case spv::OpName:
			case spv::OpMemberName:
			case spv::OpLine:
			case spv::OpDecorate:
			case spv::OpMemberDecorate:
			case spv::OpGroupDecorate:
			case spv::OpGroupMemberDecorate:
			case spv::OpDecorationGroup:
				// Handled in second pass once all IDs are in place
				break;
			default:
			{
				// we should not crash if we don't recognise/handle an opcode - this may happen because of
				// extended SPIR-V or simply custom instructions we don't recognise.
				RDCWARN("Unhandled opcode %s - result ID will be missing", ToStr::Get(op.opcode).c_str());
				if(curBlock)
					curBlock->instructions.push_back(&op);
				break;
			}
		}

		it += WordCount;
	}

	// second pass now that we have all ids set up, apply decorations/names/etc
	it = 5;
	while(it < spirvLength)
	{
		uint16_t WordCount = spirv[it]>>spv::WordCountShift;
		spv::Op op = spv::Op(spirv[it]&spv::OpCodeMask);

		switch(op)
		{
			case spv::OpName:
			{
				SPVInstruction *varInst = module.GetByID(spirv[it+1]);
				RDCASSERT(varInst);

				varInst->str = (const char *)&spirv[it+2];

				// strip any 'encoded type' information from function names
				if(varInst->opcode == spv::OpFunction)
				{
					size_t bracket = varInst->str.find('(');
					if(bracket != string::npos)
						varInst->str = varInst->str.substr(0, bracket);
				}

				if(varInst->type)
					varInst->type->name = varInst->str;
				break;
			}
			case spv::OpMemberName:
			{
				SPVInstruction *varInst = module.GetByID(spirv[it+1]);
				RDCASSERT(varInst && varInst->type && varInst->type->type == SPVTypeData::eStruct);
				uint32_t memIdx = spirv[it+2];
				RDCASSERT(memIdx < varInst->type->children.size());
				varInst->type->children[memIdx].second = (const char *)&spirv[it+3];
				break;
			}
			case spv::OpLine:
			{
				SPVInstruction *varInst = module.GetByID(spirv[it+1]);
				RDCASSERT(varInst);

				SPVInstruction *fileInst = module.GetByID(spirv[it+2]);
				RDCASSERT(fileInst);

				varInst->source.filename = fileInst->str;
				varInst->source.line = spirv[it+3];
				varInst->source.col = spirv[it+4];
				break;
			}
			case spv::OpDecorate:
			{
				SPVInstruction *inst = module.GetByID(spirv[it+1]);
				RDCASSERT(inst);

				SPVDecoration d;
				d.decoration = spv::Decoration(spirv[it+2]);
				
				// TODO this isn't enough for all decorations
				RDCASSERT(WordCount <= 4);
				if(WordCount > 3)
					d.val = spirv[it+3];

				inst->decorations.push_back(d);
				break;
			}
			case spv::OpMemberDecorate:
			{
				SPVInstruction *structInst = module.GetByID(spirv[it+1]);
				RDCASSERT(structInst && structInst->type && structInst->type->type == SPVTypeData::eStruct);

				uint32_t memberIdx = spirv[it+2];
				RDCASSERT(memberIdx < structInst->type->children.size());

				SPVDecoration d;
				d.decoration = spv::Decoration(spirv[it+3]);
				
				// TODO this isn't enough for all decorations
				RDCASSERT(WordCount <= 5);
				if(WordCount > 4)
					d.val = spirv[it+4];

				structInst->type->decorations[memberIdx].push_back(d);
				break;
			}
			case spv::OpGroupDecorate:
			case spv::OpGroupMemberDecorate:
			case spv::OpDecorationGroup:
				// TODO
				RDCBREAK();
				break;
			default:
				break;
		}

		it += WordCount;
	}

	struct SortByVarClass
	{
		bool operator () (const SPVInstruction *a, const SPVInstruction *b)
		{
			RDCASSERT(a->var && b->var);

			return a->var->storage < b->var->storage;
		}
	};

	std::sort(module.globals.begin(), module.globals.end(), SortByVarClass());
}

template<>
string ToStrHelper<false, spv::Op>::Get(const spv::Op &el)
{
	switch(el)
	{
		case spv::OpNop:                                      return "Nop";
		case spv::OpUndef:                                    return "Undef";
		case spv::OpSourceContinued:                          return "SourceContinued";
		case spv::OpSource:                                   return "Source";
		case spv::OpSourceExtension:                          return "SourceExtension";
		case spv::OpName:                                     return "Name";
		case spv::OpMemberName:                               return "MemberName";
		case spv::OpString:                                   return "String";
		case spv::OpLine:                                     return "Line";
		case spv::OpExtension:                                return "Extension";
		case spv::OpExtInstImport:                            return "ExtInstImport";
		case spv::OpExtInst:                                  return "ExtInst";
		case spv::OpMemoryModel:                              return "MemoryModel";
		case spv::OpEntryPoint:                               return "EntryPoint";
		case spv::OpExecutionMode:                            return "ExecutionMode";
		case spv::OpCapability:                               return "Capability";
		case spv::OpTypeVoid:                                 return "TypeVoid";
		case spv::OpTypeBool:                                 return "TypeBool";
		case spv::OpTypeInt:                                  return "TypeInt";
		case spv::OpTypeFloat:                                return "TypeFloat";
		case spv::OpTypeVector:                               return "TypeVector";
		case spv::OpTypeMatrix:                               return "TypeMatrix";
		case spv::OpTypeImage:                                return "TypeImage";
		case spv::OpTypeSampler:                              return "TypeSampler";
		case spv::OpTypeSampledImage:                         return "TypeSampledImage";
		case spv::OpTypeArray:                                return "TypeArray";
		case spv::OpTypeRuntimeArray:                         return "TypeRuntimeArray";
		case spv::OpTypeStruct:                               return "TypeStruct";
		case spv::OpTypeOpaque:                               return "TypeOpaque";
		case spv::OpTypePointer:                              return "TypePointer";
		case spv::OpTypeFunction:                             return "TypeFunction";
		case spv::OpTypeEvent:                                return "TypeEvent";
		case spv::OpTypeDeviceEvent:                          return "TypeDeviceEvent";
		case spv::OpTypeReserveId:                            return "TypeReserveId";
		case spv::OpTypeQueue:                                return "TypeQueue";
		case spv::OpTypePipe:                                 return "TypePipe";
		case spv::OpTypeForwardPointer:                       return "TypeForwardPointer";
		case spv::OpConstantTrue:                             return "ConstantTrue";
		case spv::OpConstantFalse:                            return "ConstantFalse";
		case spv::OpConstant:                                 return "Constant";
		case spv::OpConstantComposite:                        return "ConstantComposite";
		case spv::OpConstantSampler:                          return "ConstantSampler";
		case spv::OpConstantNull:                             return "ConstantNull";
		case spv::OpSpecConstantTrue:                         return "SpecConstantTrue";
		case spv::OpSpecConstantFalse:                        return "SpecConstantFalse";
		case spv::OpSpecConstant:                             return "SpecConstant";
		case spv::OpSpecConstantComposite:                    return "SpecConstantComposite";
		case spv::OpSpecConstantOp:                           return "SpecConstantOp";
		case spv::OpFunction:                                 return "Function";
		case spv::OpFunctionParameter:                        return "FunctionParameter";
		case spv::OpFunctionEnd:                              return "FunctionEnd";
		case spv::OpFunctionCall:                             return "FunctionCall";
		case spv::OpVariable:                                 return "Variable";
		case spv::OpImageTexelPointer:                        return "ImageTexelPointer";
		case spv::OpLoad:                                     return "Load";
		case spv::OpStore:                                    return "Store";
		case spv::OpCopyMemory:                               return "CopyMemory";
		case spv::OpCopyMemorySized:                          return "CopyMemorySized";
		case spv::OpAccessChain:                              return "AccessChain";
		case spv::OpInBoundsAccessChain:                      return "InBoundsAccessChain";
		case spv::OpPtrAccessChain:                           return "PtrAccessChain";
		case spv::OpArrayLength:                              return "ArrayLength";
		case spv::OpGenericPtrMemSemantics:                   return "GenericPtrMemSemantics";
		case spv::OpInBoundsPtrAccessChain:                   return "InBoundsPtrAccessChain";
		case spv::OpDecorate:                                 return "Decorate";
		case spv::OpMemberDecorate:                           return "MemberDecorate";
		case spv::OpDecorationGroup:                          return "DecorationGroup";
		case spv::OpGroupDecorate:                            return "GroupDecorate";
		case spv::OpGroupMemberDecorate:                      return "GroupMemberDecorate";
		case spv::OpVectorExtractDynamic:                     return "VectorExtractDynamic";
		case spv::OpVectorInsertDynamic:                      return "VectorInsertDynamic";
		case spv::OpVectorShuffle:                            return "VectorShuffle";
		case spv::OpCompositeConstruct:                       return "CompositeConstruct";
		case spv::OpCompositeExtract:                         return "CompositeExtract";
		case spv::OpCompositeInsert:                          return "CompositeInsert";
		case spv::OpCopyObject:                               return "CopyObject";
		case spv::OpTranspose:                                return "Transpose";
		case spv::OpSampledImage:                             return "SampledImage";
		case spv::OpImageSampleImplicitLod:                   return "ImageSampleImplicitLod";
		case spv::OpImageSampleExplicitLod:                   return "ImageSampleExplicitLod";
		case spv::OpImageSampleDrefImplicitLod:               return "ImageSampleDrefImplicitLod";
		case spv::OpImageSampleDrefExplicitLod:               return "ImageSampleDrefExplicitLod";
		case spv::OpImageSampleProjImplicitLod:               return "ImageSampleProjImplicitLod";
		case spv::OpImageSampleProjExplicitLod:               return "ImageSampleProjExplicitLod";
		case spv::OpImageSampleProjDrefImplicitLod:           return "ImageSampleProjDrefImplicitLod";
		case spv::OpImageSampleProjDrefExplicitLod:           return "ImageSampleProjDrefExplicitLod";
		case spv::OpImageFetch:                               return "ImageFetch";
		case spv::OpImageGather:                              return "ImageGather";
		case spv::OpImageDrefGather:                          return "ImageDrefGather";
		case spv::OpImageRead:                                return "ImageRead";
		case spv::OpImageWrite:                               return "ImageWrite";
		case spv::OpImageQueryFormat:                         return "ImageQueryFormat";
		case spv::OpImageQueryOrder:                          return "ImageQueryOrder";
		case spv::OpImageQuerySizeLod:                        return "ImageQuerySizeLod";
		case spv::OpImageQuerySize:                           return "ImageQuerySize";
		case spv::OpImageQueryLod:                            return "ImageQueryLod";
		case spv::OpImageQueryLevels:                         return "ImageQueryLevels";
		case spv::OpImageQuerySamples:                        return "ImageQuerySamples";
		case spv::OpConvertFToU:                              return "ConvertFToU";
		case spv::OpConvertFToS:                              return "ConvertFToS";
		case spv::OpConvertSToF:                              return "ConvertSToF";
		case spv::OpConvertUToF:                              return "ConvertUToF";
		case spv::OpUConvert:                                 return "UConvert";
		case spv::OpSConvert:                                 return "SConvert";
		case spv::OpFConvert:                                 return "FConvert";
		case spv::OpQuantizeToF16:                            return "QuantizeToF16";
		case spv::OpConvertPtrToU:                            return "ConvertPtrToU";
		case spv::OpSatConvertSToU:                           return "SatConvertSToU";
		case spv::OpSatConvertUToS:                           return "SatConvertUToS";
		case spv::OpConvertUToPtr:                            return "ConvertUToPtr";
		case spv::OpPtrCastToGeneric:                         return "PtrCastToGeneric";
		case spv::OpGenericCastToPtr:                         return "GenericCastToPtr";
		case spv::OpGenericCastToPtrExplicit:                 return "GenericCastToPtrExplicit";
		case spv::OpBitcast:                                  return "Bitcast";
		case spv::OpSNegate:                                  return "SNegate";
		case spv::OpFNegate:                                  return "FNegate";
		case spv::OpIAdd:                                     return "IAdd";
		case spv::OpFAdd:                                     return "FAdd";
		case spv::OpISub:                                     return "ISub";
		case spv::OpFSub:                                     return "FSub";
		case spv::OpIMul:                                     return "IMul";
		case spv::OpFMul:                                     return "FMul";
		case spv::OpUDiv:                                     return "UDiv";
		case spv::OpSDiv:                                     return "SDiv";
		case spv::OpFDiv:                                     return "FDiv";
		case spv::OpUMod:                                     return "UMod";
		case spv::OpSRem:                                     return "SRem";
		case spv::OpSMod:                                     return "SMod";
		case spv::OpFRem:                                     return "FRem";
		case spv::OpFMod:                                     return "FMod";
		case spv::OpVectorTimesScalar:                        return "VectorTimesScalar";
		case spv::OpMatrixTimesScalar:                        return "MatrixTimesScalar";
		case spv::OpVectorTimesMatrix:                        return "VectorTimesMatrix";
		case spv::OpMatrixTimesVector:                        return "MatrixTimesVector";
		case spv::OpMatrixTimesMatrix:                        return "MatrixTimesMatrix";
		case spv::OpOuterProduct:                             return "OuterProduct";
		case spv::OpDot:                                      return "Dot";
		case spv::OpIAddCarry:                                return "IAddCarry";
		case spv::OpISubBorrow:                               return "ISubBorrow";
		case spv::OpUMulExtended:                             return "UMulExtended";
		case spv::OpSMulExtended:                             return "SMulExtended";
		case spv::OpAny:                                      return "Any";
		case spv::OpAll:                                      return "All";
		case spv::OpIsNan:                                    return "IsNan";
		case spv::OpIsInf:                                    return "IsInf";
		case spv::OpIsFinite:                                 return "IsFinite";
		case spv::OpIsNormal:                                 return "IsNormal";
		case spv::OpSignBitSet:                               return "SignBitSet";
		case spv::OpLessOrGreater:                            return "LessOrGreater";
		case spv::OpOrdered:                                  return "Ordered";
		case spv::OpUnordered:                                return "Unordered";
		case spv::OpLogicalEqual:                             return "LogicalEqual";
		case spv::OpLogicalNotEqual:                          return "LogicalNotEqual";
		case spv::OpLogicalOr:                                return "LogicalOr";
		case spv::OpLogicalAnd:                               return "LogicalAnd";
		case spv::OpLogicalNot:                               return "LogicalNot";
		case spv::OpSelect:                                   return "Select";
		case spv::OpIEqual:                                   return "IEqual";
		case spv::OpINotEqual:                                return "INotEqual";
		case spv::OpUGreaterThan:                             return "UGreaterThan";
		case spv::OpSGreaterThan:                             return "SGreaterThan";
		case spv::OpUGreaterThanEqual:                        return "UGreaterThanEqual";
		case spv::OpSGreaterThanEqual:                        return "SGreaterThanEqual";
		case spv::OpULessThan:                                return "ULessThan";
		case spv::OpSLessThan:                                return "SLessThan";
		case spv::OpULessThanEqual:                           return "ULessThanEqual";
		case spv::OpSLessThanEqual:                           return "SLessThanEqual";
		case spv::OpFOrdEqual:                                return "FOrdEqual";
		case spv::OpFUnordEqual:                              return "FUnordEqual";
		case spv::OpFOrdNotEqual:                             return "FOrdNotEqual";
		case spv::OpFUnordNotEqual:                           return "FUnordNotEqual";
		case spv::OpFOrdLessThan:                             return "FOrdLessThan";
		case spv::OpFUnordLessThan:                           return "FUnordLessThan";
		case spv::OpFOrdGreaterThan:                          return "FOrdGreaterThan";
		case spv::OpFUnordGreaterThan:                        return "FUnordGreaterThan";
		case spv::OpFOrdLessThanEqual:                        return "FOrdLessThanEqual";
		case spv::OpFUnordLessThanEqual:                      return "FUnordLessThanEqual";
		case spv::OpFOrdGreaterThanEqual:                     return "FOrdGreaterThanEqual";
		case spv::OpFUnordGreaterThanEqual:                   return "FUnordGreaterThanEqual";
		case spv::OpShiftRightLogical:                        return "ShiftRightLogical";
		case spv::OpShiftRightArithmetic:                     return "ShiftRightArithmetic";
		case spv::OpShiftLeftLogical:                         return "ShiftLeftLogical";
		case spv::OpBitwiseOr:                                return "BitwiseOr";
		case spv::OpBitwiseXor:                               return "BitwiseXor";
		case spv::OpBitwiseAnd:                               return "BitwiseAnd";
		case spv::OpNot:                                      return "Not";
		case spv::OpBitFieldInsert:                           return "BitFieldInsert";
		case spv::OpBitFieldSExtract:                         return "BitFieldSExtract";
		case spv::OpBitFieldUExtract:                         return "BitFieldUExtract";
		case spv::OpBitReverse:                               return "BitReverse";
		case spv::OpBitCount:                                 return "BitCount";
		case spv::OpDPdx:                                     return "DPdx";
		case spv::OpDPdy:                                     return "DPdy";
		case spv::OpFwidth:                                   return "Fwidth";
		case spv::OpDPdxFine:                                 return "DPdxFine";
		case spv::OpDPdyFine:                                 return "DPdyFine";
		case spv::OpFwidthFine:                               return "FwidthFine";
		case spv::OpDPdxCoarse:                               return "DPdxCoarse";
		case spv::OpDPdyCoarse:                               return "DPdyCoarse";
		case spv::OpFwidthCoarse:                             return "FwidthCoarse";
		case spv::OpEmitVertex:                               return "EmitVertex";
		case spv::OpEndPrimitive:                             return "EndPrimitive";
		case spv::OpEmitStreamVertex:                         return "EmitStreamVertex";
		case spv::OpEndStreamPrimitive:                       return "EndStreamPrimitive";
		case spv::OpControlBarrier:                           return "ControlBarrier";
		case spv::OpMemoryBarrier:                            return "MemoryBarrier";
		case spv::OpAtomicLoad:                               return "AtomicLoad";
		case spv::OpAtomicStore:                              return "AtomicStore";
		case spv::OpAtomicExchange:                           return "AtomicExchange";
		case spv::OpAtomicCompareExchange:                    return "AtomicCompareExchange";
		case spv::OpAtomicCompareExchangeWeak:                return "AtomicCompareExchangeWeak";
		case spv::OpAtomicIIncrement:                         return "AtomicIIncrement";
		case spv::OpAtomicIDecrement:                         return "AtomicIDecrement";
		case spv::OpAtomicIAdd:                               return "AtomicIAdd";
		case spv::OpAtomicISub:                               return "AtomicISub";
		case spv::OpAtomicSMin:                               return "AtomicSMin";
		case spv::OpAtomicUMin:                               return "AtomicUMin";
		case spv::OpAtomicSMax:                               return "AtomicSMax";
		case spv::OpAtomicUMax:                               return "AtomicUMax";
		case spv::OpAtomicAnd:                                return "AtomicAnd";
		case spv::OpAtomicOr:                                 return "AtomicOr";
		case spv::OpAtomicXor:                                return "AtomicXor";
		case spv::OpPhi:                                      return "Phi";
		case spv::OpLoopMerge:                                return "LoopMerge";
		case spv::OpSelectionMerge:                           return "SelectionMerge";
		case spv::OpLabel:                                    return "Label";
		case spv::OpBranch:                                   return "Branch";
		case spv::OpBranchConditional:                        return "BranchConditional";
		case spv::OpSwitch:                                   return "Switch";
		case spv::OpKill:                                     return "Kill";
		case spv::OpReturn:                                   return "Return";
		case spv::OpReturnValue:                              return "ReturnValue";
		case spv::OpUnreachable:                              return "Unreachable";
		case spv::OpLifetimeStart:                            return "LifetimeStart";
		case spv::OpLifetimeStop:                             return "LifetimeStop";
		case spv::OpAsyncGroupCopy:                           return "AsyncGroupCopy";
		case spv::OpWaitGroupEvents:                          return "WaitGroupEvents";
		case spv::OpGroupAll:                                 return "GroupAll";
		case spv::OpGroupAny:                                 return "GroupAny";
		case spv::OpGroupBroadcast:                           return "GroupBroadcast";
		case spv::OpGroupIAdd:                                return "GroupIAdd";
		case spv::OpGroupFAdd:                                return "GroupFAdd";
		case spv::OpGroupFMin:                                return "GroupFMin";
		case spv::OpGroupUMin:                                return "GroupUMin";
		case spv::OpGroupSMin:                                return "GroupSMin";
		case spv::OpGroupFMax:                                return "GroupFMax";
		case spv::OpGroupUMax:                                return "GroupUMax";
		case spv::OpGroupSMax:                                return "GroupSMax";
		case spv::OpReadPipe:                                 return "ReadPipe";
		case spv::OpWritePipe:                                return "WritePipe";
		case spv::OpReservedReadPipe:                         return "ReservedReadPipe";
		case spv::OpReservedWritePipe:                        return "ReservedWritePipe";
		case spv::OpReserveReadPipePackets:                   return "ReserveReadPipePackets";
		case spv::OpReserveWritePipePackets:                  return "ReserveWritePipePackets";
		case spv::OpCommitReadPipe:                           return "CommitReadPipe";
		case spv::OpCommitWritePipe:                          return "CommitWritePipe";
		case spv::OpIsValidReserveId:                         return "IsValidReserveId";
		case spv::OpGetNumPipePackets:                        return "GetNumPipePackets";
		case spv::OpGetMaxPipePackets:                        return "GetMaxPipePackets";
		case spv::OpGroupReserveReadPipePackets:              return "GroupReserveReadPipePackets";
		case spv::OpGroupReserveWritePipePackets:             return "GroupReserveWritePipePackets";
		case spv::OpGroupCommitReadPipe:                      return "GroupCommitReadPipe";
		case spv::OpGroupCommitWritePipe:                     return "GroupCommitWritePipe";
		case spv::OpEnqueueMarker:                            return "EnqueueMarker";
		case spv::OpEnqueueKernel:                            return "EnqueueKernel";
		case spv::OpGetKernelNDrangeSubGroupCount:            return "GetKernelNDrangeSubGroupCount";
		case spv::OpGetKernelNDrangeMaxSubGroupSize:          return "GetKernelNDrangeMaxSubGroupSize";
		case spv::OpGetKernelWorkGroupSize:                   return "GetKernelWorkGroupSize";
		case spv::OpGetKernelPreferredWorkGroupSizeMultiple:  return "GetKernelPreferredWorkGroupSizeMultiple";
		case spv::OpRetainEvent:                              return "RetainEvent";
		case spv::OpReleaseEvent:                             return "ReleaseEvent";
		case spv::OpCreateUserEvent:                          return "CreateUserEvent";
		case spv::OpIsValidEvent:                             return "IsValidEvent";
		case spv::OpSetUserEventStatus:                       return "SetUserEventStatus";
		case spv::OpCaptureEventProfilingInfo:                return "CaptureEventProfilingInfo";
		case spv::OpGetDefaultQueue:                          return "GetDefaultQueue";
		case spv::OpBuildNDRange:                             return "BuildNDRange";
		case spv::OpImageSparseSampleImplicitLod:             return "ImageSparseSampleImplicitLod";
		case spv::OpImageSparseSampleExplicitLod:             return "ImageSparseSampleExplicitLod";
		case spv::OpImageSparseSampleDrefImplicitLod:         return "ImageSparseSampleDrefImplicitLod";
		case spv::OpImageSparseSampleDrefExplicitLod:         return "ImageSparseSampleDrefExplicitLod";
		case spv::OpImageSparseSampleProjImplicitLod:         return "ImageSparseSampleProjImplicitLod";
		case spv::OpImageSparseSampleProjExplicitLod:         return "ImageSparseSampleProjExplicitLod";
		case spv::OpImageSparseSampleProjDrefImplicitLod:     return "ImageSparseSampleProjDrefImplicitLod";
		case spv::OpImageSparseSampleProjDrefExplicitLod:     return "ImageSparseSampleProjDrefExplicitLod";
		case spv::OpImageSparseFetch:                         return "ImageSparseFetch";
		case spv::OpImageSparseGather:                        return "ImageSparseGather";
		case spv::OpImageSparseDrefGather:                    return "ImageSparseDrefGather";
		case spv::OpImageSparseTexelsResident:                return "ImageSparseTexelsResident";
		case spv::OpNoLine:                                   return "NoLine";
		case spv::OpAtomicFlagTestAndSet:                     return "AtomicFlagTestAndSet";
		case spv::OpAtomicFlagClear:                          return "AtomicFlagClear";
		default: break;
	}

	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::SourceLanguage>::Get(const spv::SourceLanguage &el)
{
	switch(el)
	{
		case spv::SourceLanguageUnknown:    return "Unknown";
		case spv::SourceLanguageESSL:       return "ESSL";
		case spv::SourceLanguageGLSL:       return "GLSL";
		case spv::SourceLanguageOpenCL_C:   return "OpenCL C";
		case spv::SourceLanguageOpenCL_CPP: return "OpenCL C++";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::Capability>::Get(const spv::Capability &el)
{
	switch(el)
	{
		case spv::CapabilityMatrix:                                    return "Matrix";
    case spv::CapabilityShader:                                    return "Shader";
    case spv::CapabilityGeometry:                                  return "Geometry";
    case spv::CapabilityTessellation:                              return "Tessellation";
    case spv::CapabilityAddresses:                                 return "Addresses";
    case spv::CapabilityLinkage:                                   return "Linkage";
    case spv::CapabilityKernel:                                    return "Kernel";
    case spv::CapabilityVector16:                                  return "Vector16";
    case spv::CapabilityFloat16Buffer:                             return "Float16Buffer";
    case spv::CapabilityFloat16:                                   return "Float16";
    case spv::CapabilityFloat64:                                   return "Float64";
    case spv::CapabilityInt64:                                     return "Int64";
    case spv::CapabilityInt64Atomics:                              return "Int64Atomics";
    case spv::CapabilityImageBasic:                                return "ImageBasic";
    case spv::CapabilityImageReadWrite:                            return "ImageReadWrite";
    case spv::CapabilityImageMipmap:                               return "ImageMipmap";
    case spv::CapabilityImageSRGBWrite:                            return "ImageSRGBWrite";
    case spv::CapabilityPipes:                                     return "Pipes";
    case spv::CapabilityGroups:                                    return "Groups";
    case spv::CapabilityDeviceEnqueue:                             return "DeviceEnqueue";
    case spv::CapabilityLiteralSampler:                            return "LiteralSampler";
    case spv::CapabilityAtomicStorage:                             return "AtomicStorage";
    case spv::CapabilityInt16:                                     return "Int16";
    case spv::CapabilityTessellationPointSize:                     return "TessellationPointSize";
    case spv::CapabilityGeometryPointSize:                         return "GeometryPointSize";
    case spv::CapabilityImageGatherExtended:                       return "ImageGatherExtended";
    case spv::CapabilityStorageImageExtendedFormats:               return "StorageImageExtendedFormats";
    case spv::CapabilityStorageImageMultisample:                   return "StorageImageMultisample";
    case spv::CapabilityUniformBufferArrayDynamicIndexing:         return "UniformBufferArrayDynamicIndexing";
    case spv::CapabilitySampledImageArrayDynamicIndexing:          return "SampledImageArrayDynamicIndexing";
    case spv::CapabilityStorageBufferArrayDynamicIndexing:         return "StorageBufferArrayDynamicIndexing";
    case spv::CapabilityStorageImageArrayDynamicIndexing:          return "StorageImageArrayDynamicIndexing";
    case spv::CapabilityClipDistance:                              return "ClipDistance";
    case spv::CapabilityCullDistance:                              return "CullDistance";
    case spv::CapabilityImageCubeArray:                            return "ImageCubeArray";
    case spv::CapabilitySampleRateShading:                         return "SampleRateShading";
    case spv::CapabilityImageRect:                                 return "ImageRect";
    case spv::CapabilitySampledRect:                               return "SampledRect";
    case spv::CapabilityGenericPointer:                            return "GenericPointer";
    case spv::CapabilityInt8:                                      return "Int8";
    case spv::CapabilityInputTarget:                               return "InputTarget";
    case spv::CapabilitySparseResidency:                           return "SparseResidency";
    case spv::CapabilityMinLod:                                    return "MinLod";
    case spv::CapabilitySampled1D:                                 return "Sampled1D";
    case spv::CapabilityImage1D:                                   return "Image1D";
    case spv::CapabilitySampledCubeArray:                          return "SampledCubeArray";
    case spv::CapabilitySampledBuffer:                             return "SampledBuffer";
    case spv::CapabilityImageBuffer:                               return "ImageBuffer";
    case spv::CapabilityImageMSArray:                              return "ImageMSArray";
    case spv::CapabilityAdvancedFormats:                           return "AdvancedFormats";
    case spv::CapabilityImageQuery:                                return "ImageQuery";
    case spv::CapabilityDerivativeControl:                         return "DerivativeControl";
    case spv::CapabilityInterpolationFunction:                     return "InterpolationFunction";
    case spv::CapabilityTransformFeedback:                         return "TransformFeedback";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::ExecutionMode>::Get(const spv::ExecutionMode &el)
{
	switch(el)
	{
    case spv::ExecutionModeInvocations:                    return "Invocations";
    case spv::ExecutionModeSpacingEqual:                   return "SpacingEqual";
    case spv::ExecutionModeSpacingFractionalEven:          return "SpacingFractionalEven";
    case spv::ExecutionModeSpacingFractionalOdd:           return "SpacingFractionalOdd";
    case spv::ExecutionModeVertexOrderCw:                  return "VertexOrderCw";
    case spv::ExecutionModeVertexOrderCcw:                 return "VertexOrderCcw";
    case spv::ExecutionModePixelCenterInteger:             return "PixelCenterInteger";
    case spv::ExecutionModeOriginUpperLeft:                return "OriginUpperLeft";
    case spv::ExecutionModeOriginLowerLeft:                return "OriginLowerLeft";
    case spv::ExecutionModeEarlyFragmentTests:             return "EarlyFragmentTests";
    case spv::ExecutionModePointMode:                      return "PointMode";
    case spv::ExecutionModeXfb:                            return "Xfb";
    case spv::ExecutionModeDepthReplacing:                 return "DepthReplacing";
    case spv::ExecutionModeDepthAny:                       return "DepthAny";
    case spv::ExecutionModeDepthGreater:                   return "DepthGreater";
    case spv::ExecutionModeDepthLess:                      return "DepthLess";
    case spv::ExecutionModeDepthUnchanged:                 return "DepthUnchanged";
    case spv::ExecutionModeLocalSize:                      return "LocalSize";
    case spv::ExecutionModeLocalSizeHint:                  return "LocalSizeHint";
    case spv::ExecutionModeInputPoints:                    return "InputPoints";
    case spv::ExecutionModeInputLines:                     return "InputLines";
    case spv::ExecutionModeInputLinesAdjacency:            return "InputLinesAdjacency";
    case spv::ExecutionModeInputTriangles:                 return "InputTriangles";
    case spv::ExecutionModeInputTrianglesAdjacency:        return "InputTrianglesAdjacency";
    case spv::ExecutionModeInputQuads:                     return "InputQuads";
    case spv::ExecutionModeInputIsolines:                  return "InputIsolines";
    case spv::ExecutionModeOutputVertices:                 return "OutputVertices";
    case spv::ExecutionModeOutputPoints:                   return "OutputPoints";
    case spv::ExecutionModeOutputLineStrip:                return "OutputLineStrip";
    case spv::ExecutionModeOutputTriangleStrip:            return "OutputTriangleStrip";
    case spv::ExecutionModeVecTypeHint:                    return "VecTypeHint";
    case spv::ExecutionModeContractionOff:                 return "ContractionOff";
    case spv::ExecutionModeIndependentForwardProgress:     return "IndependentForwardProgress";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::AddressingModel>::Get(const spv::AddressingModel &el)
{
	switch(el)
	{
		case spv::AddressingModelLogical:    return "Logical";
		case spv::AddressingModelPhysical32: return "Physical (32-bit)";
		case spv::AddressingModelPhysical64: return "Physical (64-bit)";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::MemoryModel>::Get(const spv::MemoryModel &el)
{
	switch(el)
	{
		case spv::MemoryModelSimple:   return "Simple";
		case spv::MemoryModelGLSL450:  return "GLSL450";
		case spv::MemoryModelOpenCL:   return "OpenCL";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::ExecutionModel>::Get(const spv::ExecutionModel &el)
{
	switch(el)
	{
		case spv::ExecutionModelVertex:    return "Vertex Shader";
		case spv::ExecutionModelTessellationControl: return "Tess. Control Shader";
		case spv::ExecutionModelTessellationEvaluation: return "Tess. Eval Shader";
		case spv::ExecutionModelGeometry:  return "Geometry Shader";
		case spv::ExecutionModelFragment:  return "Fragment Shader";
		case spv::ExecutionModelGLCompute: return "Compute Shader";
		case spv::ExecutionModelKernel:    return "Kernel";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::Decoration>::Get(const spv::Decoration &el)
{
	switch(el)
	{
		case spv::DecorationRelaxedPrecision:                           return "RelaxedPrecision";
		case spv::DecorationSpecId:                                     return "SpecId";
		case spv::DecorationBlock:                                      return "Block";
		case spv::DecorationBufferBlock:                                return "BufferBlock";
		case spv::DecorationRowMajor:                                   return "RowMajor";
		case spv::DecorationColMajor:                                   return "ColMajor";
		case spv::DecorationArrayStride:                                return "ArrayStride";
		case spv::DecorationMatrixStride:                               return "MatrixStride";
		case spv::DecorationGLSLShared:                                 return "GLSLShared";
		case spv::DecorationGLSLPacked:                                 return "GLSLPacked";
		case spv::DecorationCPacked:                                    return "CPacked";
		case spv::DecorationBuiltIn:                                    return "BuiltIn";
		case spv::DecorationSmooth:                                     return "Smooth";
		case spv::DecorationNoPerspective:                              return "NoPerspective";
		case spv::DecorationFlat:                                       return "Flat";
		case spv::DecorationPatch:                                      return "Patch";
		case spv::DecorationCentroid:                                   return "Centroid";
		case spv::DecorationSample:                                     return "Sample";
		case spv::DecorationInvariant:                                  return "Invariant";
		case spv::DecorationRestrict:                                   return "Restrict";
		case spv::DecorationAliased:                                    return "Aliased";
		case spv::DecorationVolatile:                                   return "Volatile";
		case spv::DecorationConstant:                                   return "Constant";
		case spv::DecorationCoherent:                                   return "Coherent";
		case spv::DecorationNonWritable:                                return "NonWritable";
		case spv::DecorationNonReadable:                                return "NonReadable";
		case spv::DecorationUniform:                                    return "Uniform";
		case spv::DecorationSaturatedConversion:                        return "SaturatedConversion";
		case spv::DecorationStream:                                     return "Stream";
		case spv::DecorationLocation:                                   return "Location";
		case spv::DecorationComponent:                                  return "Component";
		case spv::DecorationIndex:                                      return "Index";
		case spv::DecorationBinding:                                    return "Binding";
		case spv::DecorationDescriptorSet:                              return "DescriptorSet";
		case spv::DecorationOffset:                                     return "Offset";
		case spv::DecorationXfbBuffer:                                  return "XfbBuffer";
		case spv::DecorationXfbStride:                                  return "XfbStride";
		case spv::DecorationFuncParamAttr:                              return "FuncParamAttr";
		case spv::DecorationFPRoundingMode:                             return "FPRoundingMode";
		case spv::DecorationFPFastMathMode:                             return "FPFastMathMode";
		case spv::DecorationLinkageAttributes:                          return "LinkageAttributes";
		case spv::DecorationNoContraction:                              return "NoContraction";
		case spv::DecorationInputTargetIndex:                           return "InputTargetIndex";
		case spv::DecorationAlignment:                                  return "Alignment";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::Dim>::Get(const spv::Dim &el)
{
	switch(el)
	{
    case spv::Dim1D:     return "1D";
    case spv::Dim2D:     return "2D";
    case spv::Dim3D:     return "3D";
    case spv::DimCube:   return "Cube";
    case spv::DimRect:   return "Rect";
		case spv::DimBuffer: return "Buffer";
		default: break;
	}
	
	return StringFormat::Fmt("{%u}D", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::StorageClass>::Get(const spv::StorageClass &el)
{
	switch(el)
	{
		case spv::StorageClassUniformConstant:    return "UniformConstant";
		case spv::StorageClassInput:              return "Input";
		case spv::StorageClassUniform:            return "Uniform";
		case spv::StorageClassOutput:             return "Output";
		case spv::StorageClassWorkgroupLocal:     return "WorkgroupLocal";
		case spv::StorageClassWorkgroupGlobal:    return "WorkgroupGlobal";
		case spv::StorageClassPrivateGlobal:      return "PrivateGlobal";
		case spv::StorageClassFunction:           return "Function";
		case spv::StorageClassGeneric:            return "Generic";
		case spv::StorageClassPushConstant:       return "PushConstant";
		case spv::StorageClassAtomicCounter:      return "AtomicCounter";
		case spv::StorageClassImage:              return "Image";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::ImageFormat>::Get(const spv::ImageFormat &el)
{
	switch(el)
	{
		case spv::ImageFormatUnknown:        return "Unknown";
		case spv::ImageFormatRgba32f:        return "RGBA32f";
		case spv::ImageFormatRgba16f:        return "RGBA16f";
		case spv::ImageFormatR32f:           return "R32f";
		case spv::ImageFormatRgba8:          return "RGBA8";
		case spv::ImageFormatRgba8Snorm:     return "RGBA8SNORM";
		case spv::ImageFormatRg32f:          return "RG32F";
		case spv::ImageFormatRg16f:          return "RG16F";
		case spv::ImageFormatR11fG11fB10f:   return "R11FG11FB10F";
		case spv::ImageFormatR16f:           return "R16F";
		case spv::ImageFormatRgba16:         return "RGBA16";
		case spv::ImageFormatRgb10A2:        return "RGB10A2";
		case spv::ImageFormatRg16:           return "RG16";
		case spv::ImageFormatRg8:            return "RG8";
		case spv::ImageFormatR16:            return "R16";
		case spv::ImageFormatR8:             return "R8";
		case spv::ImageFormatRgba16Snorm:    return "RGBA16SNORM";
		case spv::ImageFormatRg16Snorm:      return "RG16SNORM";
		case spv::ImageFormatRg8Snorm:       return "RG8SNORM";
		case spv::ImageFormatR16Snorm:       return "R16SNORM";
		case spv::ImageFormatR8Snorm:        return "R8SNORM";
		case spv::ImageFormatRgba32i:        return "RGBA32I";
		case spv::ImageFormatRgba16i:        return "RGBA16I";
		case spv::ImageFormatRgba8i:         return "RGBA8I";
		case spv::ImageFormatR32i:           return "R32I";
		case spv::ImageFormatRg32i:          return "RG32I";
		case spv::ImageFormatRg16i:          return "RG16I";
		case spv::ImageFormatRg8i:           return "RG8I";
		case spv::ImageFormatR16i:           return "R16I";
		case spv::ImageFormatR8i:            return "R8I";
		case spv::ImageFormatRgba32ui:       return "RGBA32UI";
		case spv::ImageFormatRgba16ui:       return "RGBA16UI";
		case spv::ImageFormatRgba8ui:        return "RGBA8UI";
		case spv::ImageFormatR32ui:          return "R32UI";
		case spv::ImageFormatRgb10a2ui:      return "RGB10A2UI";
		case spv::ImageFormatRg32ui:         return "RG32UI";
		case spv::ImageFormatRg16ui:         return "RG16UI";
		case spv::ImageFormatRg8ui:          return "RG8UI";
		case spv::ImageFormatR16ui:          return "R16UI";
		case spv::ImageFormatR8ui:           return "R8UI";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::BuiltIn>::Get(const spv::BuiltIn &el)
{
	switch(el)
	{
		case spv::BuiltInPosition:                         return "Position";
		case spv::BuiltInPointSize:                        return "PointSize";
		case spv::BuiltInClipDistance:                     return "ClipDistance";
		case spv::BuiltInCullDistance:                     return "CullDistance";
		case spv::BuiltInVertexId:                         return "VertexId";
		case spv::BuiltInInstanceId:                       return "InstanceId";
		case spv::BuiltInPrimitiveId:                      return "PrimitiveId";
		case spv::BuiltInInvocationId:                     return "InvocationId";
		case spv::BuiltInLayer:                            return "Layer";
		case spv::BuiltInViewportIndex:                    return "ViewportIndex";
		case spv::BuiltInTessLevelOuter:                   return "TessLevelOuter";
		case spv::BuiltInTessLevelInner:                   return "TessLevelInner";
		case spv::BuiltInTessCoord:                        return "TessCoord";
		case spv::BuiltInPatchVertices:                    return "PatchVertices";
		case spv::BuiltInFragCoord:                        return "FragCoord";
		case spv::BuiltInPointCoord:                       return "PointCoord";
		case spv::BuiltInFrontFacing:                      return "FrontFacing";
		case spv::BuiltInSampleId:                         return "SampleId";
		case spv::BuiltInSamplePosition:                   return "SamplePosition";
		case spv::BuiltInSampleMask:                       return "SampleMask";
		case spv::BuiltInFragColor:                        return "FragColor";
		case spv::BuiltInFragDepth:                        return "FragDepth";
		case spv::BuiltInHelperInvocation:                 return "HelperInvocation";
		case spv::BuiltInNumWorkgroups:                    return "NumWorkgroups";
		case spv::BuiltInWorkgroupSize:                    return "WorkgroupSize";
		case spv::BuiltInWorkgroupId:                      return "WorkgroupId";
		case spv::BuiltInLocalInvocationId:                return "LocalInvocationId";
		case spv::BuiltInGlobalInvocationId:               return "GlobalInvocationId";
		case spv::BuiltInLocalInvocationIndex:             return "LocalInvocationIndex";
		case spv::BuiltInWorkDim:                          return "WorkDim";
		case spv::BuiltInGlobalSize:                       return "GlobalSize";
		case spv::BuiltInEnqueuedWorkgroupSize:            return "EnqueuedWorkgroupSize";
		case spv::BuiltInGlobalOffset:                     return "GlobalOffset";
		case spv::BuiltInGlobalLinearId:                   return "GlobalLinearId";
		case spv::BuiltInWorkgroupLinearId:                return "WorkgroupLinearId";
		case spv::BuiltInSubgroupSize:                     return "SubgroupSize";
		case spv::BuiltInSubgroupMaxSize:                  return "SubgroupMaxSize";
		case spv::BuiltInNumSubgroups:                     return "NumSubgroups";
		case spv::BuiltInNumEnqueuedSubgroups:             return "NumEnqueuedSubgroups";
		case spv::BuiltInSubgroupId:                       return "SubgroupId";
		case spv::BuiltInSubgroupLocalInvocationId:        return "SubgroupLocalInvocationId";
		case spv::BuiltInVertexIndex:                      return "VertexIndex";
		case spv::BuiltInInstanceIndex:                    return "InstanceIndex";
		default: break;
	}
	
	return StringFormat::Fmt("Unrecognised{%u}", (uint32_t)el);
}

template<>
string ToStrHelper<false, spv::FunctionControlMask>::Get(const spv::FunctionControlMask &el)
{
	string ret;

	if(el & spv::FunctionControlInlineMask)     ret += ", Inline";
	if(el & spv::FunctionControlDontInlineMask) ret += ", DontInline";
	if(el & spv::FunctionControlPureMask)       ret += ", Pure";
	if(el & spv::FunctionControlConstMask)      ret += ", Const";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}

template<>
string ToStrHelper<false, spv::SelectionControlMask>::Get(const spv::SelectionControlMask &el)
{
	string ret;

	if(el & spv::SelectionControlFlattenMask)     ret += ", Flatten";
	if(el & spv::SelectionControlDontFlattenMask) ret += ", DontFlatten";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}

template<>
string ToStrHelper<false, spv::LoopControlMask>::Get(const spv::LoopControlMask &el)
{
	string ret;

	if(el & spv::LoopControlUnrollMask)     ret += ", Unroll";
	if(el & spv::LoopControlDontUnrollMask) ret += ", DontUnroll";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}

template<>
string ToStrHelper<false, spv::MemoryAccessMask>::Get(const spv::MemoryAccessMask &el)
{
	string ret;
	
	if(el & spv::MemoryAccessVolatileMask)     ret += ", Volatile";
	if(el & spv::MemoryAccessAlignedMask) ret += ", Aligned";
	
	if(!ret.empty())
		ret = ret.substr(2);

	return ret;
}
