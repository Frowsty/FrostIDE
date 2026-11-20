// SPDX-License-Identifier: Apache-2.0
// Read exact schema declaration text through the common decoder, bounded to its physical project.
#include "engine_json_schema_reader.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_file.hpp"
namespace frost::app {
Result<text::Snapshot> readEngineSchemaDeclaration(io::IFileSystem& fs,std::string_view root,std::string_view path) {
    auto physicalRoot=fs.physicalPath(root),physical=fs.physicalPath(path);
    if(!physicalRoot)return std::unexpected(physicalRoot.error());if(!physical)return std::unexpected(physical.error());
    const auto rootKey=lang::documentKey(*physicalRoot),key=lang::documentKey(*physical);
    if(!key.starts_with(rootKey)||key.size()<=rootKey.size()||(key[rootKey.size()]!='/'&&key[rootKey.size()]!='\\'))
        return fail(ErrorCode::PermissionDenied,"Engine schema declaration is outside the selected physical project");
    auto status=fs.status(*physical);if(!status)return std::unexpected(status.error());
    if(status->type!=io::FileType::Regular)return fail(ErrorCode::InvalidArgument,"Engine schema declaration is not a regular file");
    text::LoadOptions options;options.maxFileBytes=2*1024*1024;options.buffer.maxBufferBytes=2*1024*1024;
    auto loaded=text::loadTextFile(fs,*physical,options);if(!loaded)return std::unexpected(loaded.error());
    if(loaded->binaryLikely||!loaded->encoding.fidelity.lossless())return fail(ErrorCode::Corrupt,"Engine schema declaration text is not losslessly decoded");
    auto afterRoot=fs.physicalPath(root),after=fs.physicalPath(path);
    if(!afterRoot||!after||lang::documentKey(*afterRoot)!=rootKey||lang::documentKey(*after)!=key)
        return fail(ErrorCode::Busy,"Engine schema declaration identity changed during capture");
    return loaded->buffer.snapshot();
}
}
