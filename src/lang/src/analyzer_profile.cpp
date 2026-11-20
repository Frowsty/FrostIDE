// SPDX-License-Identifier: Apache-2.0
// Typed severity/profile settings shared by native analysis and existing inspection filtering.
#include "frost/lang/analyzer.hpp"
#include "frost/lang/uri.hpp"
namespace frost::lang {
Result<InspectionProfile> analyzerProfile(std::span<const std::string> entries) {
    InspectionProfile profile;profile.name="Frost analyzer";
    for(const auto& entry:entries) {
        const auto equal=entry.rfind('=');
        if(equal==std::string::npos || equal==0 || equal+1==entry.size()) return fail(ErrorCode::InvalidArgument,"Analyzer profile entries require check-pattern=severity");
        const auto name=entry.substr(0,equal),level=entry.substr(equal+1);
        InspectionSeverity severity;
        if(level=="error") severity=InspectionSeverity::Error;
        else if(level=="warning") severity=InspectionSeverity::Warning;
        else if(level=="weak-warning") severity=InspectionSeverity::WeakWarning;
        else if(level=="info" || level=="note") severity=InspectionSeverity::Info;
        else if(level=="off") severity=InspectionSeverity::Disabled;
        else return fail(ErrorCode::InvalidArgument,"Unknown analyzer severity: "+level);
        profile.rules.push_back({name,severity,severity!=InspectionSeverity::Disabled});
    }
    return profile;
}
Result<void> validateAnalyzerInputs(io::IFileSystem& fs,const AnalyzerReport& report) {
    for(const auto& image:report.inputs) {
        auto physical=fs.physicalPath(image.path);if(!physical || documentKey(*physical)!=documentKey(image.physicalPath)) return fail(ErrorCode::Busy,"Analyzer input identity changed: "+image.path);
        if(image.overlay) continue; // Caller separately retains and validates its exact editor snapshots.
        auto bytes=fs.readFileSnapshot(image.path,64U*1024U*1024U);
        if(!bytes || *bytes!=image.bytes) return fail(ErrorCode::Busy,"Analyzer input changed: "+image.path);
    }
    for(const auto& path:report.missing) {auto exists=fs.exists(path);if(!exists || *exists) return fail(ErrorCode::Busy,"Analyzer missing input appeared: "+path);}
    for(const auto& path:report.directories) {auto status=fs.status(path);if(!status || status->type!=io::FileType::Directory) return fail(ErrorCode::Busy,"Analyzer directory input changed: "+path);}
    return {};
}
}
