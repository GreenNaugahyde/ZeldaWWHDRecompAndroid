// GLSL -> SPIR-V with glslang. The decompiler emits one source for OpenGL and Vulkan; the VULKAN
// macro (predefined by glslang for Vulkan input, GL_KHR_vulkan_glsl) selects the Vulkan bindings:
// one descriptor set per stage, as in Cemu's Vulkan renderer.
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <string>
#include <vector>

#include "runtime.h"
#include "vk.h"

namespace gfx {

void shader_compiler_init() {
    static bool done = (glslang::InitializeProcess(), true);
    (void)done;
}

bool compile_glsl_stage(const char* src, EShLanguage stage, std::vector<uint32_t>& spirv, std::string& log);

bool compile_glsl(const char* src, bool vertex, std::vector<uint32_t>& spirv, std::string& log) {
    return compile_glsl_stage(src, vertex ? EShLangVertex : EShLangFragment, spirv, log);
}

bool compile_glsl_compute(const char* src, std::vector<uint32_t>& spirv, std::string& log) {
    return compile_glsl_stage(src, EShLangCompute, spirv, log);
}

bool compile_glsl_stage(const char* src, EShLanguage stage, std::vector<uint32_t>& spirv, std::string& log) {
    glslang::TShader sh(stage);
    sh.setStrings(&src, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!sh.parse(GetDefaultResources(), 450, ECoreProfile, false, false, msgs)) {
        log = sh.getInfoLog();
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs)) {
        log = prog.getInfoLog();
        return false;
    }
    glslang::SpvOptions opt;
    opt.generateDebugInfo = false;
    opt.disableOptimizer = true;  // the driver optimizes; keeps translation fast
    spirv.clear();
    glslang::GlslangToSpv(*prog.getIntermediate(stage), spirv, &opt);
    return !spirv.empty();
}

}  // namespace gfx
