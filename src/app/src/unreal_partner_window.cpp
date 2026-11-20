// SPDX-License-Identifier: Apache-2.0
// Independently authored IDE client for an explicitly selected user-local partner.
#include "unreal_partner_window.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/io/path.hpp"
#include <QTcpSocket>
#include <QHostAddress>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QCloseEvent>
#include <algorithm>
namespace frost::app {
UnrealPartnerWindow::UnrealPartnerWindow(std::string projectFile,project::TrustStore& trust,QWidget* parent)
    :QDialog(parent),m_project(std::move(projectFile)),m_root(io::parentPath(m_project)),m_trust(trust) {
    setAttribute(Qt::WA_DeleteOnClose);setWindowTitle(tr("Unreal local partner"));resize(950,650);
    auto* layout=new QVBoxLayout(this);
    auto* note=new QLabel(tr("Connect only to the partner you configured locally. Blueprint rows cover actual loaded graph calls reported by that partner, not every asset. No Editor plug-in is installed by this action."),this);
    note->setWordWrap(true);layout->addWidget(note);
    auto* form=new QFormLayout();m_port=new QSpinBox(this);m_port->setRange(1024,65535);m_port->setValue(40000);
    m_secret=new QLineEdit(this);m_secret->setEchoMode(QLineEdit::Password);m_secret->setMaxLength(64);
    form->addRow(tr("Loopback port"),m_port);form->addRow(tr("Session token"),m_secret);layout->addLayout(form);
    auto* row=new QHBoxLayout();auto* connectButton=new QPushButton(tr("Connect"),this);m_connectButton=connectButton;auto* disconnectButton=new QPushButton(tr("Disconnect"),this);
    row->addWidget(connectButton);row->addWidget(disconnectButton);
    connect(connectButton,&QPushButton::clicked,this,[this]{beginSession();});connect(disconnectButton,&QPushButton::clicked,this,[this]{endSession(tr("Disconnected"));});
    for(const auto& item:{std::pair{tr("Play"),"pie.play"},std::pair{tr("Stop PIE"),"pie.stop"},std::pair{tr("Pause"),"pie.pause"},std::pair{tr("Resume"),"pie.resume"},std::pair{tr("Refresh loaded Blueprint calls"),"blueprints.loadedCalls"}}){
        auto* button=new QPushButton(item.first,this);row->addWidget(button);m_controls.push_back(button);
        connect(button,&QPushButton::clicked,this,[this,operation=std::string(item.second)]{send(operation);});
    }
    layout->addLayout(row);m_status=new QLabel(tr("Disconnected; no partner data"),this);m_status->setWordWrap(true);layout->addWidget(m_status);
    m_state=new QLabel(tr("PIE state unavailable"),this);layout->addWidget(m_state);
    auto* filter=new QLineEdit(this);filter->setPlaceholderText(tr("Filter observed Blueprint or function object"));layout->addWidget(filter);
    m_calls=new QTableWidget(0,3,this);m_calls->setObjectName(QStringLiteral("unrealBlueprintCalls"));
    m_calls->setHorizontalHeaderLabels({tr("Blueprint object"),tr("Function object"),tr("Actual graph node")});
    m_calls->setEditTriggers(QAbstractItemView::NoEditTriggers);m_calls->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);layout->addWidget(m_calls);
    m_calls->setToolTip(tr("Snapshot from the last explicit refresh. Refresh after graph edits."));
    connect(filter,&QLineEdit::textChanged,this,[this](const QString& query){
        for(int row=0;row<m_calls->rowCount();++row){bool match=query.isEmpty();for(int column=0;column<3;++column)if(const auto* item=m_calls->item(row,column))match=match||item->text().contains(query,Qt::CaseInsensitive);m_calls->setRowHidden(row,!match);}
    });
    m_logs=new QPlainTextEdit(this);m_logs->setReadOnly(true);m_logs->document()->setMaximumBlockCount(1000);layout->addWidget(m_logs);
    m_watch.setInterval(100);connect(&m_watch,&QTimer::timeout,this,[this]{checkSession();});updateControls();
}
UnrealPartnerWindow::~UnrealPartnerWindow(){m_watch.stop();if(m_socket)m_socket->abort();std::fill(m_token.begin(),m_token.end(),'\0');}
bool UnrealPartnerWindow::admitted() const {const auto now=m_trust.snapshot(m_root);return m_socket&&now.trusted&&now.revision==m_revision;}
void UnrealPartnerWindow::updateControls(){for(auto* widget:m_controls)widget->setEnabled(m_ready&&admitted());m_port->setEnabled(!m_socket);m_secret->setEnabled(!m_socket);m_connectButton->setEnabled(!m_socket);}
void UnrealPartnerWindow::closeEvent(QCloseEvent* event){endSession(tr("Disconnected"));QDialog::closeEvent(event);}
void UnrealPartnerWindow::endSession(const QString& reason){
    ++m_epoch;m_watch.stop();m_ready=false;m_pending.clear();m_input.clear();m_initialState.reset();
    if(m_socket){auto* socket=m_socket.data();m_socket=nullptr;QObject::disconnect(socket,nullptr,this,nullptr);socket->abort();socket->deleteLater();}
    std::fill(m_token.begin(),m_token.end(),'\0');m_token.clear();m_secret->clear();m_calls->setRowCount(0);m_logs->clear();m_state->setText(tr("PIE state unavailable"));m_status->setText(reason);updateControls();
}
void UnrealPartnerWindow::beginSession(){
    if(m_socket)return;
    const auto token=m_secret->text().toLatin1().toStdString();
    const auto request=lang::unrealPartnerRequest(1,token,m_project,"state");
    if(!request){m_status->setText(QString::fromStdString(request.error().message()));return;}
    const auto permission=m_trust.snapshot(m_root);if(!permission.trusted){m_status->setText(tr("Trust this project before connecting to its local partner."));return;}
    endSession(tr("Connecting to selected loopback partner…"));m_token=token;m_revision=permission.revision;m_nextId=0;
    const auto epoch=m_epoch;auto* socket=new QTcpSocket(this);m_socket=socket;socket->setReadBufferSize(65536);m_elapsed.start();m_lastFrame=0;m_watch.start();updateControls();
    connect(socket,&QTcpSocket::connected,this,[this,epoch]{if(epoch==m_epoch&&admitted())send("state");});
    connect(socket,&QTcpSocket::readyRead,this,[this,epoch]{if(epoch==m_epoch)readFrames();});
    connect(socket,&QTcpSocket::disconnected,this,[this,epoch]{if(epoch==m_epoch)endSession(tr("Local partner disconnected; reconnect explicitly."));});
    connect(socket,&QTcpSocket::errorOccurred,this,[this,epoch](QAbstractSocket::SocketError){if(epoch==m_epoch)endSession(tr("Local partner connection failed."));});
    socket->connectToHost(QHostAddress(QStringLiteral("127.0.0.1")),static_cast<quint16>(m_port->value()));
}
void UnrealPartnerWindow::send(std::string operation){
    if(!admitted()){endSession(tr("Project trust changed; reconnect explicitly."));return;}
    if(!m_socket||m_socket->state()!=QAbstractSocket::ConnectedState||(!m_ready&&operation!="state"))return;
    if(m_pending.size()>=16||m_socket->bytesToWrite()>65536){endSession(tr("Local partner request backlog exceeded."));return;}
    auto request=lang::unrealPartnerRequest(++m_nextId,m_token,m_project,operation);
    if(!request){endSession(tr("Local partner request refused."));return;}
    m_pending.emplace(m_nextId,Pending{std::move(operation),m_elapsed.elapsed()+10000});
    if(m_socket->write(request->data(),static_cast<qint64>(request->size()))<0)endSession(tr("Local partner write failed."));
}
void UnrealPartnerWindow::readFrames(){
    if(!admitted()){endSession(tr("Project trust changed; partner data discarded."));return;}
    m_input+=m_socket->readAll();if(m_input.size()>65536){endSession(tr("Local partner frame limit exceeded."));return;}
    for(int count=0;count<16;++count){const auto newline=m_input.indexOf('\n');if(newline<0)break;
        const auto bytes=m_input.left(newline);m_input.remove(0,newline+1);
        auto event=lang::parseUnrealPartnerEvent(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size())),m_project);
        if(!event){endSession(tr("Local partner sent invalid or incompatible data."));return;}
        m_lastFrame=m_elapsed.elapsed();receive(*event);if(!m_socket)return;
    }
    if(m_input.contains('\n'))QTimer::singleShot(0,this,[this,epoch=m_epoch]{if(epoch==m_epoch)readFrames();});
}
void UnrealPartnerWindow::receive(const lang::UnrealPartnerEvent& event){
    if(!admitted()){endSession(tr("Project trust changed; partner data discarded."));return;}
    if(event.kind=="reply"){
        const auto pending=m_pending.find(event.id);if(pending==m_pending.end()){endSession(tr("Local partner reply does not match a pending request."));return;}
        const auto operation=pending->second.operation;m_pending.erase(pending);
        if(!event.calls.empty()||!event.scope.empty())if(operation!="blueprints.loadedCalls"){endSession(tr("Unexpected Blueprint call payload."));return;}
        if(!m_ready){if(operation!="state"||event.status!="ok"){endSession(tr("Local partner handshake was refused."));return;}m_ready=true;updateControls();if(m_initialState){auto state=*m_initialState;m_initialState.reset();receive(state);}if(!m_socket||!admitted())return;}
        if(operation=="blueprints.loadedCalls"){
            m_calls->setRowCount(0);
            if(event.status=="ok"&&event.scope!="loaded-blueprint-graphs"){endSession(tr("Local partner omitted the Blueprint observation scope."));return;}
            if(event.status=="ok")for(const auto& call:event.calls){const int row=m_calls->rowCount();m_calls->insertRow(row);
                m_calls->setItem(row,0,new QTableWidgetItem(QString::fromStdString(call.blueprint)));m_calls->setItem(row,1,new QTableWidgetItem(QString::fromStdString(call.function)));m_calls->setItem(row,2,new QTableWidgetItem(QString::fromStdString(call.node)));}
        }
        m_status->setText(tr("Partner reply: %1 — %2").arg(QString::fromStdString(operation),QString::fromStdString(event.status)));
    }else if(event.kind=="state"){
        if(!m_ready){m_initialState=event;return;}
        m_state->setText(event.playing?(event.paused?tr("Observed PIE state: paused"):tr("Observed PIE state: playing")):tr("Observed PIE state: stopped"));
    }else if(m_ready&&event.kind=="log")m_logs->appendPlainText(QString::fromUtf8(event.message.data(),static_cast<qsizetype>(event.message.size())));
    else if(m_ready&&event.kind=="droppedLogs")m_logs->appendPlainText(tr("Partner reports %1 dropped log messages.").arg(static_cast<qulonglong>(event.droppedLogs)));
}
void UnrealPartnerWindow::checkSession(){
    if(!admitted()){endSession(tr("Project trust changed; reconnect explicitly."));return;}
    const auto now=m_elapsed.elapsed();
    if((!m_ready&&now>10000)||(m_ready&&now-m_lastFrame>10000)){endSession(tr("Local partner timed out; no current data."));return;}
    for(const auto& [id,pending]:m_pending)if(now>pending.deadline){(void)id;endSession(tr("Local partner request timed out."));return;}
}
} // namespace frost::app
