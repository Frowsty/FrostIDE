// SPDX-License-Identifier: Apache-2.0
// Framed remote output, explicit remote cleanup acknowledgement, and cancellation/lease ownership.
#include "frost/build/remote_process.hpp"
#include "frost/rpc/json.hpp"
#include "frost/io/async_process.hpp"
#include "remote_guard.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
namespace frost::build {
namespace {
using namespace std::chrono_literals;
std::atomic<std::uint64_t> sequence{0};
std::string hex(std::string_view bytes) {
    constexpr char digits[]="0123456789abcdef";
    std::string result;result.reserve(bytes.size()*2);
    for (char byte : bytes) {
        const auto c = static_cast<unsigned char>(byte);
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}
Result<std::string> unhex(std::string_view text) {
    if(text.size()>16384||text.size()%2!=0)return fail(ErrorCode::Corrupt,"Remote output packet size is invalid");
    const auto value=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    std::string bytes;bytes.reserve(text.size()/2);
    for(std::size_t i=0;i<text.size();i+=2){const auto a=value(text[i]),b=value(text[i+1]);if(a<0||b<0)return fail(ErrorCode::Corrupt,"Remote output packet is not hexadecimal");bytes+=static_cast<char>((a<<4)|b);}return bytes;
}
class RemoteProcess final : public io::IProcess {
public:
    RemoteProcess(std::unique_ptr<io::IProcess> process,std::string token,RemoteExecutionOptions options,bool input)
        :m_process(std::move(process)),m_token(std::move(token)),m_options(std::move(options)),m_acceptInput(input) {
        m_outReader=std::thread([this]{readProtocol();});
        m_errReader=std::thread([this]{readTransportErrors();});
        m_monitor=std::thread([this]{monitor();});
        m_controller=std::thread([this]{control();});
    }
    ~RemoteProcess() override {
        static_cast<void>(terminate());
        if(!m_process->wait(m_options.stopTimeout+2s))static_cast<void>(m_process->terminate());
        m_process->abandonOutput();
        {std::lock_guard lock(m_mutex);m_shutdown=true;m_changed.notify_all();}
        if(m_monitor.joinable())m_monitor.join();
        if(m_controller.joinable())m_controller.join();
        if(m_outReader.joinable())m_outReader.join();
        if(m_errReader.joinable())m_errReader.join();
    }
    Result<void> started() {
        std::unique_lock lock(m_mutex);
        if(!m_changed.wait_for(lock,m_options.startupTimeout,[&]{return m_started||m_error||m_transportExited;})) {
            m_error=Error(ErrorCode::Timeout,"Remote guardian startup timed out");
            stopLocked();
        }
        if(m_error)return std::unexpected(*m_error);
        if(!m_started)return fail(ErrorCode::IoError,"Remote guardian did not acknowledge a child launch: "+m_transportError);
        return {};
    }
    io::ProcessId pid() const noexcept override {return m_process->pid();} // Local transport PID, never mislabeled as remote PID.
    Result<void> writeStdin(std::string_view bytes) override {
        {std::lock_guard lock(m_mutex);if(!m_acceptInput||m_inputClosed)return fail(ErrorCode::InvalidArgument,"Remote workload stdin is closed");}
        for(std::size_t offset=0;offset<bytes.size();offset+=4096) {
            auto result=send(rpc::Json::object().set("input",hex(bytes.substr(offset,4096))));
            if(!result)return result;
        }
        return {};
    }
    void closeStdin() noexcept override {
        {std::lock_guard lock(m_mutex);if(m_inputClosed)return;m_inputClosed=true;}
        // Child EOF is a protocol message; the heartbeat/control pipe stays alive until exit.
        static_cast<void>(send(rpc::Json::object().set("close",true)));
    }
    Result<std::size_t> readStdout(std::span<char> buffer) override {return read(buffer,false);}
    Result<std::size_t> readStderr(std::span<char> buffer) override {return read(buffer,true);}
    Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout) override {
        std::unique_lock lock(m_mutex);
        const auto finished=[&]{return m_transportExited&&m_outEnd&&m_errEnd;};
        if(timeout) {if(!m_changed.wait_for(lock,*timeout,finished))return fail(ErrorCode::Timeout,"Remote workload is still running");}
        else m_changed.wait(lock,finished);
        if(m_error)return std::unexpected(*m_error);
        if(!m_remoteExit||!m_cleanup)return fail(ErrorCode::IoError,"Remote cleanup acknowledgement unavailable; remote lease expiry may still be pending");
        if(m_transportCode!=0)return fail(ErrorCode::IoError,"Remote transport failed after guardian acknowledgement: "+std::to_string(m_transportCode));
        return io::ExitStatus{m_exitCode,0};
    }
    Result<void> terminate() override {
        std::lock_guard lock(m_mutex);
        if(m_remoteExit&&m_cleanup)return {};
        stopLocked();return {};
    }
    Result<io::ProcessResult> communicate(std::string_view input,IClock::Duration timeout) override {return communicateUntil(input,timeout);}
    Result<io::ProcessResult> communicateUntil(std::string_view input,std::optional<IClock::Duration> timeout) override {
        io::ProcessResult result;std::optional<Error> outputError;std::mutex errorMutex;
        auto drain=[&](bool error) {
            std::array<char,8192> buffer{};
            while(true) {
                auto got=error?readStderr(buffer):readStdout(buffer);
                if(!got){std::lock_guard lock(errorMutex);outputError=got.error();break;}
                if(*got==0)break;
                auto& bytes=error?result.stderrBytes:result.stdoutBytes;
                if(bytes.size()+*got>32U*1024U*1024U) {
                    {std::lock_guard lock(errorMutex);outputError=Error(ErrorCode::OutOfRange,"Remote captured output exceeds 32 MiB");}
                    static_cast<void>(terminate());continue;
                }
                bytes.append(buffer.data(),*got);
            }
        };
        std::thread out([&]{drain(false);}),err([&]{drain(true);});
        Result<void> written;
        std::thread writer([&]{written=input.empty()?Result<void>{}:writeStdin(input);closeStdin();});
        auto status=wait(timeout);
        if(!status) {static_cast<void>(terminate());static_cast<void>(wait(m_options.stopTimeout+2s));m_process->abandonOutput();}
        writer.join();out.join();err.join();
        if(!written)return std::unexpected(written.error());
        if(!status)return std::unexpected(status.error());
        if(outputError)return std::unexpected(*outputError);
        result.exitCode=status->exitCode;result.signal=status->signal;return result;
    }
    Result<std::vector<io::ContainedProcess>> containedProcesses() override {
        std::lock_guard lock(m_mutex);
        if(m_remoteExit&&m_cleanup)return std::vector<io::ContainedProcess>{};
        return fail(ErrorCode::Unsupported,"Remote process inventory is not a local PID namespace");
    }
    Result<void> releaseContainer() override {return fail(ErrorCode::Unsupported,"Remote workloads always retain owned descendant cleanup");}
    void abandonOutput() noexcept override {
        {std::lock_guard lock(m_mutex);m_discardOutput=true;m_stdout.clear();m_stderr.clear();m_outBytes=m_errBytes=0;m_changed.notify_all();}
        m_process->abandonOutput();
    }
private:
    void stopLocked() {
        if(!m_stop){m_stop=true;m_stopAt=std::chrono::steady_clock::now();}m_changed.notify_all();
    }
    Result<void> send(rpc::Json message) {
        message.set("version",1).set("token",m_token);
        std::lock_guard lock(m_writeMutex);
        return m_process->writeStdin(message.dump()+"\n");
    }
    void failure(Error error) {
        std::lock_guard lock(m_mutex);if(!m_error)m_error=std::move(error);stopLocked();
    }
    Result<std::size_t> read(std::span<char> buffer,bool error) {
        if(buffer.empty())return std::size_t{0};
        std::unique_lock lock(m_mutex);auto& queue=error?m_stderr:m_stdout;auto& bytes=error?m_errBytes:m_outBytes;
        m_changed.wait(lock,[&]{return !queue.empty()||(error?m_errEnd:m_outEnd)||m_discardOutput;});
        if(queue.empty())return std::size_t{0};
        auto& front=queue.front();const auto count=std::min(buffer.size(),front.size());
        std::copy_n(front.begin(),count,buffer.begin());front.erase(0,count);bytes-=count;if(front.empty())queue.pop_front();
        m_changed.notify_all();return count;
    }
    void output(std::string bytes,bool error) {
        std::unique_lock lock(m_mutex);auto& count=error?m_errBytes:m_outBytes;
        m_changed.wait(lock,[&]{return count+bytes.size()<=4U*1024U*1024U||m_shutdown||m_discardOutput;});
        if(m_shutdown||m_discardOutput)return;
        count+=bytes.size();(error?m_stderr:m_stdout).push_back(std::move(bytes));m_changed.notify_all();
    }
    void packet(std::string_view line) {
        if(line.size()>65536){failure(Error(ErrorCode::OutOfRange,"Remote guardian packet exceeds 64 KiB"));return;}
        auto parsed=rpc::parseJson(line);
        if(!parsed||parsed->integerAt("version")!=1||parsed->stringAt("token")!=m_token) {
            failure(Error(ErrorCode::Corrupt,"Remote guardian protocol/version/identity mismatch"));return;
        }
        const auto event=parsed->stringAt("event");
        if(event=="started") {
            const auto pid=parsed->integerAt("pid");
            std::lock_guard lock(m_mutex);
            if(m_started||m_remoteExit||!pid||*pid<=0){m_error=Error(ErrorCode::Corrupt,"Malformed remote launch acknowledgement");stopLocked();return;}
            m_started=true;m_changed.notify_all();return;
        }
        if(event=="stdout"||event=="stderr") {
            {std::lock_guard lock(m_mutex);if(!m_started||m_remoteExit){m_error=Error(ErrorCode::Corrupt,"Remote output outside acknowledged child lifetime");stopLocked();return;}}
            const auto* value=parsed->find("data");auto data=value&&value->asString()?unhex(*value->asString()):Result<std::string>(fail(ErrorCode::Corrupt,"Missing remote output bytes"));
            if(!data){failure(data.error());return;}output(std::move(*data),event=="stderr");return;
        }
        if(event=="error") {failure(Error(ErrorCode::IoError,"Remote guardian: "+std::string(parsed->stringAt("message"))));return;}
        if(event=="exited") {
            const auto code=parsed->integerAt("exitCode");const auto* cleanup=parsed->find("cleanup");
            std::lock_guard lock(m_mutex);
            if(!m_started||m_remoteExit||!code||*code<0||*code>255||!cleanup||!cleanup->asBool()) {
                m_error=Error(ErrorCode::Corrupt,"Malformed remote exit/cleanup acknowledgement");stopLocked();return;
            }
            m_remoteExit=true;m_cleanup=*cleanup->asBool();m_exitCode=static_cast<int>(*code);
            if(!m_cleanup){m_error=Error(ErrorCode::IoError,"Remote guardian could not confirm descendant cleanup");stopLocked();}
            m_changed.notify_all();return;
        }
        failure(Error(ErrorCode::Corrupt,"Unknown remote guardian event"));
    }
    void readProtocol() {
        std::array<char,8192> bytes{};std::string pending;
        while(true) {
            auto count=m_process->readStdout(bytes);if(!count){failure(count.error());break;}if(*count==0)break;
            pending.append(bytes.data(),*count);
            while(true){const auto end=pending.find('\n');if(end==pending.npos)break;packet(std::string_view(pending).substr(0,end));pending.erase(0,end+1);}
            if(pending.size()>65536){failure(Error(ErrorCode::OutOfRange,"Remote protocol line exceeds 64 KiB"));break;}
        }
        if(!pending.empty())failure(Error(ErrorCode::Corrupt,"Truncated remote guardian frame"));
        std::lock_guard lock(m_mutex);m_outEnd=true;m_changed.notify_all();
    }
    void readTransportErrors() {
        std::array<char,8192> bytes{};
        while(true){auto count=m_process->readStderr(bytes);if(!count){failure(count.error());break;}if(*count==0)break;
            {std::lock_guard lock(m_mutex);if(m_transportError.size()<16384)m_transportError.append(bytes.data(),std::min<std::size_t>(*count,16384-m_transportError.size()));}
            output(std::string(bytes.data(),*count),true);
        }
        std::lock_guard lock(m_mutex);m_errEnd=true;m_changed.notify_all();
    }
    void monitor() {
        while(true) {
            {std::lock_guard lock(m_mutex);if(m_shutdown){static_cast<void>(m_process->terminate());return;}}
            auto status=m_process->wait(200ms);
            if(status){std::lock_guard lock(m_mutex);m_transportExited=true;m_transportCode=status->exitCode;m_changed.notify_all();return;}
            if(status.error().code()!=ErrorCode::Timeout){failure(status.error());std::lock_guard lock(m_mutex);m_transportExited=true;m_changed.notify_all();return;}
            if(auto admitted=m_options.admit();!admitted)failure(admitted.error());
            bool kill=false;
            {std::lock_guard lock(m_mutex);kill=m_stop&&std::chrono::steady_clock::now()-m_stopAt>=m_options.stopTimeout;}
            if(kill) {
                failure(Error(ErrorCode::Timeout,"Remote Stop acknowledgement timed out; remote cleanup is unconfirmed"));
                static_cast<void>(m_process->terminate());m_process->abandonOutput();
            }
        }
    }
    void control() {
        while(true) {
            bool stop=false;
            {std::unique_lock lock(m_mutex);m_changed.wait_for(lock,1s,[&]{return m_stop||m_transportExited||m_shutdown;});
                if(m_transportExited||m_shutdown)return;stop=m_stop;}
            auto sent=send(stop?rpc::Json::object().set("stop",true):rpc::Json::object().set("ping",true));
            if(!sent)failure(sent.error());
            if(stop){std::unique_lock lock(m_mutex);m_changed.wait(lock,[&]{return m_transportExited||m_shutdown;});return;}
        }
    }
    std::unique_ptr<io::IProcess> m_process;
    std::string m_token;RemoteExecutionOptions m_options;
    std::mutex m_mutex,m_writeMutex;std::condition_variable m_changed;
    std::thread m_outReader,m_errReader,m_monitor,m_controller;
    std::deque<std::string> m_stdout,m_stderr;std::size_t m_outBytes=0,m_errBytes=0;
    bool m_started=false,m_remoteExit=false,m_cleanup=false,m_outEnd=false,m_errEnd=false,m_transportExited=false;
    bool m_stop=false,m_shutdown=false,m_discardOutput=false,m_acceptInput=false,m_inputClosed=false;
    int m_exitCode=0,m_transportCode=0;std::string m_transportError;std::optional<Error> m_error;
    std::chrono::steady_clock::time_point m_stopAt{};
};
}
Result<std::unique_ptr<io::IProcess>> RemoteProcessLauncher::launch(const io::ProcessSpec& workload) {
    if(auto valid=validateRemoteWorkload(workload);!valid)return std::unexpected(valid.error());
    if(m_options.startupTimeout<=0ms||m_options.startupTimeout>60s||m_options.stopTimeout<=0ms||m_options.stopTimeout>30s)
        return fail(ErrorCode::InvalidArgument,"Remote startup/Stop deadlines exceed the bounded supported policy");
    if(!m_options.admit)return fail(ErrorCode::Untrusted,"Remote execution requires current project and endpoint admission");
    if(auto admitted=m_options.admit();!admitted)return std::unexpected(admitted.error());
#ifndef _WIN32
    if(m_target.transport==RemoteTransport::Wsl)return fail(ErrorCode::Unsupported,"WSL toolchains require the Windows IDE");
#endif
    if(workload.stdoutMode!=io::Stdio::Pipe||workload.stderrMode!=io::Stdio::Pipe||
        (workload.stdinMode!=io::Stdio::Pipe&&workload.stdinMode!=io::Stdio::Null))
        return fail(ErrorCode::Unsupported,"Remote workloads require captured output and piped/null stdin");
    auto spec=remoteTransportSpec(m_target,detail::kRemoteGuard);if(!spec)return std::unexpected(spec.error());
    const auto token="frost-remote-"+std::to_string(++sequence);
    auto argv=rpc::Json::array();argv.push(workload.executable);for(const auto& arg:workload.arguments)argv.push(arg);
    auto environment=rpc::Json::array();for(const auto& [name,value]:workload.environment)
        environment.push(rpc::Json::object().set("name",name).set("value",value?rpc::Json(*value):rpc::Json{}));
    const auto request=rpc::Json::object().set("version",1).set("token",token).set("argv",std::move(argv)).set("cwd",workload.workingDirectory)
        .set("environment",std::move(environment)).set("cleanEnvironment",workload.cleanEnvironment).set("lowPriority",workload.belowNormalPriority)
        .set("stdin",workload.stdinMode==io::Stdio::Pipe).dump()+"\n";
    if(request.size()>1024U*1024U)return fail(ErrorCode::OutOfRange,"Remote workload launch request exceeds 1 MiB");
    spec->belowNormalPriority=workload.belowNormalPriority;
    auto local=m_local.launch(*spec);if(!local)return std::unexpected(local.error());
    if(auto written=(*local)->writeStdin(request);!written)return std::unexpected(written.error());
    auto process=std::make_unique<RemoteProcess>(std::move(*local),token,m_options,workload.stdinMode==io::Stdio::Pipe);
    if(auto started=process->started();!started)return std::unexpected(started.error());
    if(auto admitted=m_options.admit();!admitted)return std::unexpected(admitted.error());
    return std::unique_ptr<io::IProcess>(std::move(process));
}
Result<std::unique_ptr<io::IAsyncProcess>> RemoteProcessLauncher::launchAsync(const io::ProcessSpec&,const io::AsyncContext&) {
    return fail(ErrorCode::Unsupported,"Remote control uses owned RunSession workers; async stream adaptation is not available");
}
} // namespace frost::build
