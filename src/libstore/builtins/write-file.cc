#include "nix/store/builtins.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"

namespace nix {

static void builtinWriteFile(const BuiltinBuilderContext & ctx)
{
    auto out = get(ctx.drv.outputs, "out");
    if (!out)
        throw Error("'builtin:write-file' requires an 'out' output");

    auto storePath = ctx.outputs.at("out");

    auto contentIt = ctx.drv.env.find("content");
    if (contentIt == ctx.drv.env.end())
        throw Error("'builtin:write-file' requires 'content' parameter");
    auto content = contentIt->second;

    writeFile(storePath, content);

    auto executableIt = ctx.drv.env.find("executable");
    if (executableIt != ctx.drv.env.end() && executableIt->second == "1") {
        if (chmod(storePath.c_str(), 0755) == -1)
            throw SysError("making '%1%' executable", storePath);
    }
}

static RegisterBuiltinBuilder registerWriteFile("write-file", builtinWriteFile);

} // namespace nix
