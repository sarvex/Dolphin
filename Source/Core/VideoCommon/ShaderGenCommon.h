// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <iomanip>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

/**
 * Common interface for classes that need to go through the shader generation path (GenerateVertexShader, GeneratePixelShader)
 * In particular, this includes the shader code generator (ShaderCode).
 * A different class (ShaderUid) can be used to uniquely identify each ShaderCode object.
 * More interesting things can be done with this, e.g. ShaderConstantProfile checks what shader constants are being used. This can be used to optimize buffer management.
 * Each of the ShaderCode, ShaderUid and ShaderConstantProfile child classes only implement the subset of ShaderGeneratorInterface methods that are required for the specific tasks.
 */
class ShaderGeneratorInterface
{
public:
	/*
	 * Used when the shader generator would write a piece of ShaderCode.
	 * Can be used like printf.
	 * @note In the ShaderCode implementation, this does indeed write the parameter string to an internal buffer. However, you're free to do whatever you like with the parameter.
	 */
	template<typename... Args>
	void Write(const char*, Args...) {}

	/*
	 * Returns a read pointer to the internal buffer.
	 * @note When implementing this method in a child class, you likely want to return the argument of the last SetBuffer call here
	 * @note SetBuffer() should be called before using GetBuffer().
	 */
	const char* GetBuffer() { return nullptr; }

	/*
	 * Can be used to give the object a place to write to. This should be called before using Write().
	 * @param buffer pointer to a char buffer that the object can write to
	 */
	void SetBuffer(char* buffer) { }

	/*
	 * Tells us that a specific constant range (including last_index) is being used by the shader
	 */
	inline void SetConstantsUsed(unsigned int first_index, unsigned int last_index) {}

	/*
	 * Returns a pointer to an internally stored object of the uid_data type.
	 * @warning since most child classes use the default implementation you shouldn't access this directly without adding precautions against nullptr access (e.g. via adding a dummy structure, cf. the vertex/pixel shader generators)
	 */
	template<class uid_data>
	uid_data* GetUidData() { return nullptr; }
};

/**
 * Shader UID class used to uniquely identify the ShaderCode output written in the shader generator.
 * uid_data can be any struct of parameters that uniquely identify each shader code output.
 * Unless performance is not an issue, uid_data should be tightly packed to reduce memory footprint.
 * Shader generators will write to specific uid_data fields; ShaderUid methods will only read raw u32 values from a union.
 */
template<class uid_data>
class ShaderUid : public ShaderGeneratorInterface
{
public:
	ShaderUid()
	{
		// TODO: Move to Shadergen => can be optimized out
		memset(values, 0, sizeof(values));
	}

	bool operator == (const ShaderUid& obj) const
	{
		return memcmp(this->values, obj.values, data.NumValues() * sizeof(*values)) == 0;
	}

	bool operator != (const ShaderUid& obj) const
	{
		return memcmp(this->values, obj.values, data.NumValues() * sizeof(*values)) != 0;
	}

	// determines the storage order inside STL containers
	bool operator < (const ShaderUid& obj) const
	{
		return memcmp(this->values, obj.values, data.NumValues() * sizeof(*values)) < 0;
	}

	template<class uid_data2>
	uid_data2* GetUidData() { return &data; }
	const uid_data* GetUidData() const { return &data; }
	const u8* GetUidDataRaw() const { return &values[0]; }

	size_t GetUidDataSize() const { return sizeof(values); }

private:
	union
	{
		uid_data data;
		u8 values[sizeof(uid_data)];
	};
};

class ShaderCode : public ShaderGeneratorInterface
{
public:
	ShaderCode() : buf(nullptr), write_ptr(nullptr)
	{
	}

	void Write(const char* fmt, ...)
	{
		va_list arglist;
		va_start(arglist, fmt);
		write_ptr += vsprintf(write_ptr, fmt, arglist);
		va_end(arglist);
	}

	const char* GetBuffer() { return buf; }
	void SetBuffer(char* buffer) { buf = buffer; write_ptr = buffer; }

private:
	const char* buf;
	char* write_ptr;
};

/**
 * Generates a shader constant profile which can be used to query which constants are used in a shader
 */
class ShaderConstantProfile : public ShaderGeneratorInterface
{
public:
	ShaderConstantProfile(int num_constants) { constant_usage.resize(num_constants); }

	inline void SetConstantsUsed(unsigned int first_index, unsigned int last_index)
	{
		for (unsigned int i = first_index; i < last_index + 1; ++i)
			constant_usage[i] = true;
	}

	inline bool ConstantIsUsed(unsigned int index)
	{
		// TODO: Not ready for usage yet
		return true;
		//return constant_usage[index];
	}
private:
	std::vector<bool> constant_usage; // TODO: Is vector<bool> appropriate here?
};
/**
 * Checks if there has been
 */
template<class UidT, class CodeT>
class UidChecker
{
public:
	void Invalidate()
	{
		m_shaders.clear();
		m_uids.clear();
	}

	void AddToIndexAndCheck(CodeT& new_code, const UidT& new_uid, const char* shader_type, const char* dump_prefix)
	{
		bool uid_is_indexed = std::find(m_uids.begin(), m_uids.end(), new_uid) != m_uids.end();
		if (!uid_is_indexed)
		{
			m_uids.push_back(new_uid);
			m_shaders[new_uid] = new_code.GetBuffer();
		}
		else
		{
			// uid is already in the index => check if there's a shader with the same uid but different code
			auto& old_code = m_shaders[new_uid];
			if (strcmp(old_code.c_str(), new_code.GetBuffer()) != 0)
			{
				static int num_failures = 0;

				std::string temp = StringFromFormat("%s%ssuid_mismatch_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(),
						dump_prefix, ++num_failures);

				// TODO: Should also dump uids
				std::ofstream file;
				OpenFStream(file, temp, std::ios_base::out);
				file << "Old shader code:\n" << old_code;
				file << "\n\nNew shader code:\n" << new_code.GetBuffer();
				file << "\n\nShader uid:\n";
				for (unsigned int i = 0; i < new_uid.GetUidDataSize(); ++i)
				{
					u8 value = new_uid.GetUidDataRaw()[i];
					if ((i % 4) == 0)
					{
						auto last_value = (i + 3 < new_uid.GetUidDataSize() - 1) ? i + 3 : new_uid.GetUidDataSize();
						file << std::setfill(' ') << std::dec;
						file << "Values " << std::setw(2) << i << " - " << last_value << ": ";
					}

					file << std::setw(2) << std::setfill('0') << std::hex << value << std::setw(1);
					if ((i % 4) < 3)
						file << ' ';
					else
						file << std::endl;
				}

				ERROR_LOG(VIDEO, "%s shader uid mismatch! See %s for details", shader_type, temp.c_str());
			}
		}
	}

private:
	std::map<UidT, std::string> m_shaders;
	std::vector<UidT> m_uids;
};

template<class T>
static void DefineOutputMember(T& object, API_TYPE api_type, const char* qualifier, const char* type, const char* name, int var_index, const char* semantic = "", int semantic_index = -1)
{
	if (qualifier != nullptr)
		object.Write("\t%s %s %s", qualifier, type, name);
	else
		object.Write("\t%s %s", type, name);

	if (var_index != -1)
		object.Write("%d", var_index);

	if (api_type == API_D3D && strlen(semantic) > 0)
	{
		if (semantic_index != -1)
			object.Write(" : %s%d", semantic, semantic_index);
		else
			object.Write(" : %s", semantic);
	}

	object.Write(";\n");
}

template<class T>
static inline void GenerateVSOutputMembers(T& object, API_TYPE api_type, const char* qualifier = nullptr)
{
	DefineOutputMember(object, api_type, qualifier, "float4", "pos", -1, "POSITION");
	DefineOutputMember(object, api_type, qualifier, "float4", "colors_", 0, "COLOR", 0);
	DefineOutputMember(object, api_type, qualifier, "float4", "colors_", 1, "COLOR", 1);

	for (unsigned int i = 0; i < xfmem.numTexGen.numTexGens; ++i)
		DefineOutputMember(object, api_type, qualifier, "float3", "tex", i, "TEXCOORD", i);

	DefineOutputMember(object, api_type, qualifier, "float4", "clipPos", -1, "TEXCOORD", xfmem.numTexGen.numTexGens);

	if (g_ActiveConfig.bEnablePixelLighting)
	{
		DefineOutputMember(object, api_type, qualifier, "float3", "Normal", -1, "TEXCOORD", xfmem.numTexGen.numTexGens + 1);
		DefineOutputMember(object, api_type, qualifier, "float3", "WorldPos", -1, "TEXCOORD", xfmem.numTexGen.numTexGens + 2);
	}
}

template<class T>
static inline void AssignVSOutputMembers(T& object, const char* a, const char* b)
{
	object.Write("\t%s.pos = %s.pos;\n", a, b);
	object.Write("\t%s.colors_0 = %s.colors_0;\n", a, b);
	object.Write("\t%s.colors_1 = %s.colors_1;\n", a, b);

	for (unsigned int i = 0; i < xfmem.numTexGen.numTexGens; ++i)
		object.Write("\t%s.tex%d = %s.tex%d;\n", a, i, b, i);

	object.Write("\t%s.clipPos = %s.clipPos;\n", a, b);

	if (g_ActiveConfig.bEnablePixelLighting)
	{
		object.Write("\t%s.Normal = %s.Normal;\n", a, b);
		object.Write("\t%s.WorldPos = %s.WorldPos;\n", a, b);
	}
}

// Constant variable names
#define I_COLORS        "color"
#define I_KCOLORS       "k"
#define I_ALPHA         "alphaRef"
#define I_TEXDIMS       "texdim"
#define I_ZBIAS         "czbias"
#define I_INDTEXSCALE   "cindscale"
#define I_INDTEXMTX     "cindmtx"
#define I_FOGCOLOR      "cfogcolor"
#define I_FOGI          "cfogi"
#define I_FOGF          "cfogf"
#define I_ZSLOPE        "czslope"
#define I_EFBSCALE      "cefbscale"

#define I_POSNORMALMATRIX       "cpnmtx"
#define I_PROJECTION            "cproj"
#define I_MATERIALS             "cmtrl"
#define I_LIGHTS                "clights"
#define I_TEXMATRICES           "ctexmtx"
#define I_TRANSFORMMATRICES     "ctrmtx"
#define I_NORMALMATRICES        "cnmtx"
#define I_POSTTRANSFORMMATRICES "cpostmtx"
#define I_PIXELCENTERCORRECTION "cpixelcenter"

#define I_STEREOPARAMS  "cstereo"
#define I_LINEPTPARAMS  "clinept"
#define I_TEXOFFSET     "ctexoffset"

static const char s_shader_uniforms[] =
	"\tfloat4 " I_POSNORMALMATRIX"[6];\n"
	"\tfloat4 " I_PROJECTION"[4];\n"
	"\tint4 " I_MATERIALS"[4];\n"
	"\tLight " I_LIGHTS"[8];\n"
	"\tfloat4 " I_TEXMATRICES"[24];\n"
	"\tfloat4 " I_TRANSFORMMATRICES"[64];\n"
	"\tfloat4 " I_NORMALMATRICES"[32];\n"
	"\tfloat4 " I_POSTTRANSFORMMATRICES"[64];\n"
	"\tfloat4 " I_PIXELCENTERCORRECTION";\n";
