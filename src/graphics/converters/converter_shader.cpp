#include "graphics/shader.h"
#include "resource/converter.h"
#include "core/array.h"
#include "core/debug.h"
#include "core/enum.h"
#include "core/file.h"
#include "core/hash.h"
#include "core/linear_allocator.h"
#include "core/misc.h"
#include "core/string.h"
#include "core/vector.h"

#include "gpu/enum.h"
#include "gpu/resources.h"
#include "gpu/utils.h"

#include "serialization/serializer.h"
#include "graphics/private/shader_impl.h"
#include "graphics/converters/import_shader.h"
#include "graphics/converters/shader_backend_hlsl.h"
#include "graphics/converters/shader_backend_metadata.h"
#include "graphics/converters/shader_compiler_hlsl.h"
#include "graphics/converters/shader_parser.h"
#include "graphics/converters/shader_preprocessor.h"

#include <cstring>

#define DEBUG_DUMP_SHADERS 0

#define DUMP_ESF_PATH "C:\\Dev\\tmp.esf"
#define DUMP_HLSL_PATH "C:\\Dev\\tmp.hlsl"

namespace
{
	class ConverterShader : public Resource::IConverter
	{
	public:
		ConverterShader() {}

		virtual ~ConverterShader() {}

		bool SupportsFileType(const char* fileExt, const Core::UUID& type) const override
		{
			return (type == Graphics::Shader::GetTypeUUID()) || (fileExt && strcmp(fileExt, "esf") == 0);
		}

		bool Convert(Resource::IConverterContext& context, const char* sourceFile, const char* destPath) override
		{
			auto metaData = context.GetMetaData<Graphics::MetaDataShader>();
			auto* pathResolver = context.GetPathResolver();
			char fullPath[Core::MAX_PATH_LENGTH];
			memset(fullPath, 0, sizeof(fullPath));
			pathResolver->ResolvePath(sourceFile, fullPath, sizeof(fullPath));

			char file[Core::MAX_PATH_LENGTH];
			memset(file, 0, sizeof(file));
			char path[Core::MAX_PATH_LENGTH];
			memset(path, 0, sizeof(path));
			if(!Core::FileSplitPath(fullPath, path, sizeof(path), file, sizeof(file), nullptr, 0))
			{
				context.AddError(__FILE__, __LINE__, "INTERNAL ERROR: Core::FileSplitPath failed.");
				return false;
			}

			char outFilename[Core::MAX_PATH_LENGTH];
			memset(outFilename, 0, sizeof(outFilename));
			strcat_s(outFilename, sizeof(outFilename), destPath);
			Core::FileNormalizePath(outFilename, sizeof(outFilename), true);

			bool retVal = false;

			//
			Core::File shaderFile(sourceFile, Core::FileFlags::READ, pathResolver);
			if(shaderFile)
			{
				Core::Vector<char> shaderSource;
				shaderSource.resize((i32)shaderFile.Size() + 1, '\0');
				shaderFile.Read(shaderSource.data(), shaderFile.Size());

				Graphics::ShaderPreprocessor preprocessor;

				// Setup include path to root of shader.
				preprocessor.AddInclude(path);

				if(!preprocessor.Preprocess(fullPath, shaderSource.data()))
					return false;

#if DEBUG_DUMP_SHADERS
				if(Core::FileExists(DUMP_ESF_PATH))
					Core::FileRemove(DUMP_ESF_PATH);
				if(auto outTmpFile = Core::File(DUMP_ESF_PATH, Core::FileFlags::WRITE | Core::FileFlags::CREATE))
				{
					outTmpFile.Write(preprocessor.GetOutput().c_str(), preprocessor.GetOutput().size());
				}
#endif

				// Add dependencies from preprocessor stage.
				Core::Array<char, Core::MAX_PATH_LENGTH> originalPath;
				for(const char* dep : preprocessor.GetDependencies())
				{
					if(pathResolver->OriginalPath(dep, originalPath.data(), originalPath.size()))
						context.AddDependency(originalPath.data());
					else if(Core::FileExists(dep))
						context.AddDependency(dep);
				}

				// Parse shader into an AST.
				Graphics::ShaderParser shaderParser;
				auto node = shaderParser.Parse(sourceFile, preprocessor.GetOutput().c_str());
				if(node == nullptr)
					return false;

				// Parse shader metadata from AST to determine what needs to be compiled.
				Graphics::ShaderBackendMetadata backendMetadata;
				node->Visit(&backendMetadata);

				// Gather all unique shaders referenced by techniques.
				const auto& techniques = backendMetadata.GetTechniques();
				Core::Array<Core::Set<Core::String>, (i32)GPU::ShaderType::MAX> shaders;

				for(const auto& technique : techniques)
				{
					if(technique.vs_.size() > 0)
						shaders[(i32)GPU::ShaderType::VS].insert(technique.vs_);
					if(technique.gs_.size() > 0)
						shaders[(i32)GPU::ShaderType::GS].insert(technique.gs_);
					if(technique.hs_.size() > 0)
						shaders[(i32)GPU::ShaderType::HS].insert(technique.hs_);
					if(technique.ds_.size() > 0)
						shaders[(i32)GPU::ShaderType::DS].insert(technique.ds_);
					if(technique.ps_.size() > 0)
						shaders[(i32)GPU::ShaderType::PS].insert(technique.ps_);
					if(technique.cs_.size() > 0)
						shaders[(i32)GPU::ShaderType::CS].insert(technique.cs_);
				}

				// Grab sampler states.
				const auto& samplerStates = backendMetadata.GetSamplerStates();

				struct CompileInfo
				{
					CompileInfo(const Core::String& name, const Core::String& code, const Core::String& entryPoint,
					    GPU::ShaderType type)
					    : name_(name)
					    , code_(code)
					    , entryPoint_(entryPoint)
					    , type_(type)
					{
					}

					Core::String name_;
					Core::String code_;
					Core::String entryPoint_;
					GPU::ShaderType type_;
				};

				Graphics::ShaderCompilerHLSL compilerHLSL;
				auto GenerateAndCompile = [&](const Graphics::BindingMap& bindingMap,
				    Core::Vector<CompileInfo>& outCompileInfo,
				    Core::Vector<Graphics::ShaderCompileOutput>& outCompileOutput) {
					outCompileInfo.clear();
					outCompileOutput.clear();

					// Generate HLSL for the whole ESF.
					Graphics::ShaderBackendHLSL backendHLSL(bindingMap, true);
					node->Visit(&backendHLSL);

#if DEBUG_DUMP_SHADERS
					if(Core::FileExists(DUMP_HLSL_PATH))
						Core::FileRemove(DUMP_HLSL_PATH);
					if(auto outTmpFile = Core::File(DUMP_HLSL_PATH, Core::FileFlags::WRITE | Core::FileFlags::CREATE))
					{
						outTmpFile.Write(backendHLSL.GetOutputCode().c_str(), backendHLSL.GetOutputCode().size());
					}
#endif

					// Compile HLSL.
					for(i32 idx = 0; idx < shaders.size(); ++idx)
						for(const auto& shader : shaders[idx])
							outCompileInfo.emplace_back(
							    sourceFile, backendHLSL.GetOutputCode(), shader, (GPU::ShaderType)idx);

					for(const auto& compile : outCompileInfo)
					{
						auto outCompile = compilerHLSL.Compile(
						    compile.name_.c_str(), compile.code_.c_str(), compile.entryPoint_.c_str(), compile.type_);
						if(outCompile)
						{
							outCompileOutput.emplace_back(outCompile);
						}
						else
						{
							Core::String errStr(outCompile.errorsBegin_, outCompile.errorsEnd_);
							Core::Log("%s", errStr.c_str());
							return false;
						}
					}

					return true;
				};

				// Generate and compile initial pass.
				Core::Vector<CompileInfo> compileInfo;
				Core::Vector<Graphics::ShaderCompileOutput> compileOutput;
				if(!GenerateAndCompile(Graphics::BindingMap(), compileInfo, compileOutput))
				{
					// ERROR.
					return false;
				}

				// Get list of all used bindings.
				Graphics::BindingMap usedBindings;
				const auto AddBindings = [](const Core::Vector<Graphics::ShaderBinding>& inBindings,
				    Graphics::BindingMap& outBindings, i32 bindingIdx) {
					for(const auto& binding : inBindings)
					{
						if(outBindings.find(binding.name_) == outBindings.end())
						{
							outBindings.insert(binding.name_, bindingIdx++);
						}
					}
					return bindingIdx;
				};

				i32 bindingIdx = 0;
				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.cbuffers_, usedBindings, bindingIdx);
				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.samplers_, usedBindings, bindingIdx);
				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.srvs_, usedBindings, bindingIdx);
				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.uavs_, usedBindings, bindingIdx);

				// Regenerate HLSL with only the used bindings.
				if(!GenerateAndCompile(usedBindings, compileInfo, compileOutput))
				{
					// ERROR.
					return false;
				}

				// Build set of all bindings used.
				Graphics::BindingMap cbuffers;
				Graphics::BindingMap samplers;
				Graphics::BindingMap srvs;
				Graphics::BindingMap uavs;
				bindingIdx = 0;

				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.cbuffers_, cbuffers, bindingIdx);
				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.samplers_, samplers, bindingIdx);
				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.srvs_, srvs, bindingIdx);
				for(const auto& compile : compileOutput)
					bindingIdx = AddBindings(compile.uavs_, uavs, bindingIdx);

				// Setup data ready to serialize.
				Graphics::ShaderHeader outHeader;
				outHeader.numCBuffers_ = cbuffers.size();
				outHeader.numSamplers_ = samplers.size();
				outHeader.numSRVs_ = srvs.size();
				outHeader.numUAVs_ = uavs.size();
				outHeader.numShaders_ = compileOutput.size();
				outHeader.numTechniques_ = techniques.size();
				outHeader.numSamplerStates_ = samplerStates.size();
				Core::Vector<Graphics::ShaderBindingHeader> outBindingHeaders;
				outBindingHeaders.reserve(cbuffers.size() + samplers.size() + srvs.size() + uavs.size());

				const auto PopulateoutBindingHeaders = [&outBindingHeaders](const Graphics::BindingMap& bindings) {
					for(const auto& binding : bindings)
					{
						Graphics::ShaderBindingHeader bindingHeader;
						memset(&bindingHeader, 0, sizeof(bindingHeader));
						strcpy_s(bindingHeader.name_, sizeof(bindingHeader.name_), binding.first.c_str());
						outBindingHeaders.push_back(bindingHeader);
					}
				};

				Core::Vector<Graphics::ShaderSamplerStateHeader> outSamplerStateHeaders;
				outSamplerStateHeaders.reserve(samplerStates.size());
				for(const auto& samplerState : samplerStates)
				{
					Graphics::ShaderSamplerStateHeader outSamplerState;
					strcpy_s(outSamplerState.name_, sizeof(outSamplerState.name_), samplerState.name_.c_str());
					outSamplerState.state_ = samplerState.state_;
					outSamplerStateHeaders.emplace_back(outSamplerState);
				}

				PopulateoutBindingHeaders(cbuffers);
				PopulateoutBindingHeaders(samplers);
				PopulateoutBindingHeaders(srvs);
				PopulateoutBindingHeaders(uavs);
				Core::Vector<Graphics::ShaderBytecodeHeader> outBytecodeHeaders;
				Core::Vector<Graphics::ShaderBindingMapping> outBindingMappings;
				i32 bytecodeOffset = 0;
				for(const auto& compile : compileOutput)
				{
					Graphics::ShaderBytecodeHeader bytecodeHeader;
					bytecodeHeader.numCBuffers_ = compile.cbuffers_.size();
					bytecodeHeader.numSamplers_ = compile.samplers_.size();
					bytecodeHeader.numSRVs_ = compile.srvs_.size();
					bytecodeHeader.numUAVs_ = compile.uavs_.size();
					bytecodeHeader.type_ = compile.type_;
					bytecodeHeader.offset_ = bytecodeOffset;
					bytecodeHeader.numBytes_ = (i32)(compile.byteCodeEnd_ - compile.byteCodeBegin_);

					bytecodeOffset += bytecodeHeader.numBytes_;
					outBytecodeHeaders.push_back(bytecodeHeader);

					const auto AddBindingMapping = [&](
					    const Graphics::BindingMap& bindingMap, const Core::Vector<Graphics::ShaderBinding>& bindings) {
						for(const auto& binding : bindings)
						{
							auto it = bindingMap.find(binding.name_);
							DBG_ASSERT(it != bindingMap.end());
							Graphics::ShaderBindingMapping mapping;
							mapping.binding_ = it->second;
							mapping.dstSlot_ = binding.slot_;
							outBindingMappings.push_back(mapping);
						}
					};

					AddBindingMapping(cbuffers, compile.cbuffers_);
					AddBindingMapping(samplers, compile.samplers_);
					AddBindingMapping(srvs, compile.srvs_);
					AddBindingMapping(uavs, compile.uavs_);
				};

				Core::Vector<Graphics::ShaderTechniqueHeader> outTechniqueHeaders;
				for(const auto& technique : backendMetadata.GetTechniques())
				{
					Graphics::ShaderTechniqueHeader techniqueHeader;
					memset(&techniqueHeader, 0, sizeof(techniqueHeader));
					strcpy_s(techniqueHeader.name_, sizeof(techniqueHeader.name_), technique.name_.c_str());

					auto FindShaderIdx = [&](const char* name) -> i32 {
						if(!name)
							return -1;
						i32 idx = 0;
						for(const auto& compile : compileInfo)
						{
							if(compile.entryPoint_ == name)
								return idx;
							++idx;
						}
						return -1;
					};
					techniqueHeader.vs_ = FindShaderIdx(technique.vs_.c_str());
					techniqueHeader.gs_ = FindShaderIdx(technique.gs_.c_str());
					techniqueHeader.hs_ = FindShaderIdx(technique.hs_.c_str());
					techniqueHeader.ds_ = FindShaderIdx(technique.ds_.c_str());
					techniqueHeader.ps_ = FindShaderIdx(technique.ps_.c_str());
					techniqueHeader.cs_ = FindShaderIdx(technique.cs_.c_str());
					techniqueHeader.rs_ = technique.rs_.state_;

					DBG_ASSERT(techniqueHeader.vs_ != -1 || techniqueHeader.cs_ != -1);

					outTechniqueHeaders.push_back(techniqueHeader);
				}

				auto WriteShader = [&](const char* outFilename) {
					// Write out shader data.
					if(auto outFile = Core::File(outFilename, Core::FileFlags::CREATE | Core::FileFlags::WRITE))
					{
						outFile.Write(&outHeader, sizeof(outHeader));
						if(outBindingHeaders.size() > 0)
							outFile.Write(outBindingHeaders.data(),
							    outBindingHeaders.size() * sizeof(Graphics::ShaderBindingHeader));
						if(outBytecodeHeaders.size() > 0)
							outFile.Write(outBytecodeHeaders.data(),
							    outBytecodeHeaders.size() * sizeof(Graphics::ShaderBytecodeHeader));
						if(outBindingMappings.size() > 0)
							outFile.Write(outBindingMappings.data(),
							    outBindingMappings.size() * sizeof(Graphics::ShaderBindingMapping));
						if(outTechniqueHeaders.size() > 0)
							outFile.Write(outTechniqueHeaders.data(),
							    outTechniqueHeaders.size() * sizeof(Graphics::ShaderTechniqueHeader));
						if(outSamplerStateHeaders.size() > 0)
							outFile.Write(outSamplerStateHeaders.data(),
							    outSamplerStateHeaders.size() * sizeof(Graphics::ShaderSamplerStateHeader));

						i64 outBytes = 0;
						for(const auto& compile : compileOutput)
						{
							outBytes +=
							    outFile.Write(compile.byteCodeBegin_, compile.byteCodeEnd_ - compile.byteCodeBegin_);
						}

						return true;
					}
					return false;
				};

				retVal = WriteShader(outFilename);
			}
			context.AddDependency(sourceFile);

			if(retVal)
			{
				context.AddOutput(outFilename);
			}

			// Setup metadata.
			context.SetMetaData(metaData);

			return retVal;
		}
	};
}


extern "C" {
EXPORT bool GetPlugin(struct Plugin::Plugin* outPlugin, Core::UUID uuid)
{
	bool retVal = false;

	// Fill in base info.
	if(uuid == Plugin::Plugin::GetUUID() || uuid == Resource::ConverterPlugin::GetUUID())
	{
		if(outPlugin)
		{
			outPlugin->systemVersion_ = Plugin::PLUGIN_SYSTEM_VERSION;
			outPlugin->pluginVersion_ = Resource::ConverterPlugin::PLUGIN_VERSION;
			outPlugin->uuid_ = Resource::ConverterPlugin::GetUUID();
			outPlugin->name_ = "Graphics.Shader Converter";
			outPlugin->desc_ = "Shader converter plugin.";
		}
		retVal = true;
	}

	// Fill in plugin specific.
	if(uuid == Resource::ConverterPlugin::GetUUID())
	{
		if(outPlugin)
		{
			auto* plugin = static_cast<Resource::ConverterPlugin*>(outPlugin);
			plugin->CreateConverter = []() -> Resource::IConverter* { return new ConverterShader(); };
			plugin->DestroyConverter = [](Resource::IConverter*& converter) {
				delete converter;
				converter = nullptr;
			};
		}
		retVal = true;
	}

	return retVal;
}
}
