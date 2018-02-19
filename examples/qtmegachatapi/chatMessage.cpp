#include "qmenu.h"
#include "chatMessage.h"
#include "ui_chatMessageWidget.h"
#include <QMessageBox>

const char* messageStatus[] =
{
  "Sending", "SendingManual", "ServerReceived", "ServerRejected", "Delivered", "NotSeen", "Seen"
};

ChatMessage::ChatMessage(ChatWindow *parent, megachat::MegaChatApi* mChatApi, megachat::MegaChatHandle chatId, megachat::MegaChatMessage *msg)
    : QWidget((QWidget *)parent),
      ui(new Ui::ChatMessageWidget)
{
    mChatWindow=parent;
    this->chatId=chatId;
    megaChatApi = mChatApi;
    ui->setupUi(this);
    message = msg;
    setAuthor();
    setTimestamp(message->getTimestamp());

    if(this->megaChatApi->getChatRoom(chatId)->isGroup() && message->getStatus()== megachat::MegaChatMessage::STATUS_DELIVERED)
        setStatus(megachat::MegaChatMessage::STATUS_SERVER_RECEIVED);
    else
        setStatus(message->getStatus());

    if (message->isEdited())
        markAsEdited();

    if (!msg->isManagementMessage())
    {
        switch (msg->getType())
        {
            case megachat::MegaChatMessage::TYPE_NODE_ATTACHMENT:
            {
                QString text;
                text.append(tr("[Attached Msg]"));
                mega::MegaNodeList *nodeList=message->getMegaNodeList();
                for(int i = 0; i < nodeList->size(); i++)
                {
                    text.append(tr("\n[Node]"))
                    .append("\nName: ")
                    .append(nodeList->get(i)->getName())
                    .append("\nHandle: ")
                    .append(QString::fromStdString(std::to_string(nodeList->get(i)->getHandle())))
                    .append("\nSize: ")
                    .append(QString::fromStdString(std::to_string(nodeList->get(i)->getSize())))
                    .append(" bytes");
                }
                ui->mMsgDisplay->setText(text);
                ui->mMsgDisplay->setStyleSheet("background-color: rgba(198,251,187,128)\n");
                ui->mAuthorDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
                ui->mTimestampDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
                ui->mHeader->setStyleSheet("background-color: rgba(107,144,163,128)\n");
                text.clear();
                break;
            }
            case megachat::MegaChatMessage::TYPE_CONTACT_ATTACHMENT:
            {
                QString text;
                text.append(tr("[Attached Contacts]"));
                for(unsigned int i = 0; i < message->getUsersCount(); i++)
                {
                  text.append(tr("\n[User]"))
                  .append("\nName: ")
                  .append(message->getUserName(i))
                  .append("\nEmail: ")
                  .append(message->getUserEmail(i));
                }
                ui->mMsgDisplay->setText(text);
                ui->mMsgDisplay->setStyleSheet("background-color: rgba(205,254,251,128)\n");
                ui->mAuthorDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
                ui->mTimestampDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
                ui->mHeader->setStyleSheet("background-color: rgba(107,144,163,128)\n");
                text.clear();
                break;
            }
            case megachat::MegaChatMessage::TYPE_NORMAL:
            {
                ui->mHeader->setStyleSheet("background-color: rgba(107,144,163,128)\n");
                ui->mAuthorDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
                ui->mTimestampDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
                setMessageContent(msg->getContent());
                break;
            }
        }
    }
    else
    {
        ui->mMsgDisplay->setText(managementInfoToString().c_str());
        ui->mHeader->setStyleSheet("background-color: rgba(192,123,11,128)\n");
        ui->mAuthorDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
        ui->mTimestampDisplay->setStyleSheet("color: rgba(0,0,0,128)\n");
    }

    connect(ui->mMsgDisplay, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(onMessageCtxMenu(const QPoint&)));
    updateToolTip();
    show();
}

ChatMessage::~ChatMessage()
{
    delete message;
    delete ui;
}

void ChatMessage::updateToolTip()
{
    QString tooltip;

    megachat::MegaChatHandle msgId;
    int status = message->getStatus();
    switch (status)
    {
    case megachat::MegaChatMessage::STATUS_SENDING:
        tooltip.append(tr("tempId: "));
        msgId = message->getTempId();
        break;
    case megachat::MegaChatMessage::STATUS_SENDING_MANUAL:
        tooltip.append(tr("rowId: "));
        msgId = message->getRowId();
        break;
    default:
        tooltip.append(tr("msgId: "));
        msgId = message->getMsgId();
        break;
    }

    tooltip.append(QString::fromStdString(std::to_string(msgId)))
            .append(tr("\ntype: "))
            .append(QString::fromStdString(std::to_string(message->getType())))
            .append(tr("\nuserid: "))
            .append(QString::fromStdString(std::to_string(message->getUserHandle())));
    ui->mHeader->setToolTip(tooltip);
}

QListWidgetItem *ChatMessage::getWidgetItem() const
{
    return mListWidgetItem;
}

void ChatMessage::setWidgetItem(QListWidgetItem *item)
{
    mListWidgetItem = item;
}

megachat::MegaChatMessage *ChatMessage::getMessage() const
{
    return message;
}

void ChatMessage::setMessage(megachat::MegaChatMessage *message)
{
    this->message = message;
}

void ChatMessage::setMessageContent(const char * content)
{
    ui->mMsgDisplay->setText(content);
}

std::string ChatMessage::managementInfoToString() const
{
    std::string ret;
    ret.reserve(128);

    switch (message->getType())
    {
    case megachat::MegaChatMessage::TYPE_ALTER_PARTICIPANTS:
    {
        ret.append("User ").append(std::to_string(message->getUserHandle()))
           .append((message->getPrivilege() == megachat::MegaChatRoom::PRIV_RM) ? " removed" : " added")
           .append(" user ").append(std::to_string(message->getHandleOfAction()));
        return ret;
    }
    case megachat::MegaChatMessage::TYPE_TRUNCATE:
    {
        ret.append("Chat history was truncated by user ").append(std::to_string(message->getUserHandle()));
        return ret;
    }
    case megachat::MegaChatMessage::TYPE_PRIV_CHANGE:
    {
        ret.append("User ").append(std::to_string(message->getUserHandle()))
           .append(" set privilege of user ").append(std::to_string(message->getHandleOfAction()))
           .append(" to ").append(std::to_string(message->getPrivilege()));
        return ret;
    }
    case megachat::MegaChatMessage::TYPE_CHAT_TITLE:
    {
        ret.append("User ").append(std::to_string(message->getUserHandle()))
           .append(" set chat title to '")
           .append(this->megaChatApi->getChatRoom(chatId)->getTitle())+='\'';
        return ret;
    }
    default:
        throw std::runtime_error("Message with type "+std::to_string(message->getType())+" is not a management message");
    }
}

void ChatMessage::setTimestamp(int64_t ts)
{
    QDateTime t;
    t.setTime_t(ts);
    ui->mTimestampDisplay->setText(t.toString("hh:mm:ss"));
}

void ChatMessage::setStatus(int status)
{
    if (status == megachat::MegaChatMessage::STATUS_UNKNOWN)
        ui->mStatusDisplay->setText("Invalid");
    else
    {
        ui->mStatusDisplay->setText(messageStatus[status]);
    }
}

void ChatMessage::setAuthor()
{
    if (isMine())
    {
        ui->mAuthorDisplay->setText(tr("me"));
    }
    else
    {
        const char *chatTitle = megaChatApi->getChatRoom(chatId)->getPeerFullnameByHandle(message->getUserHandle());
        ui->mAuthorDisplay->setText(tr(chatTitle));
        delete chatTitle;
    }
}

bool ChatMessage::isMine() const
{
    return (message->getUserHandle() == megaChatApi->getMyUserHandle());
}

void ChatMessage::markAsEdited()
{
    setStatus(message->getStatus());
    ui->mStatusDisplay->setText(ui->mStatusDisplay->text()+" (Edited)");
}

void ChatMessage::onMessageCtxMenu(const QPoint& point)
{
   if (isMine() && !message->isManagementMessage())
   {
        QMenu *menu = ui->mMsgDisplay->createStandardContextMenu(point);
        auto action = menu->addAction(tr("&Edit message"));
        action->setData(QVariant::fromValue(this));
        connect(action, SIGNAL(triggered()), this, SLOT(onMessageEditAction()));
        auto delAction = menu->addAction(tr("Delete message"));
        delAction->setData(QVariant::fromValue(this));
        connect(delAction, SIGNAL(triggered()), this, SLOT(onMessageDelAction()));
        menu->popup(this->mapToGlobal(point));
   }
}

void ChatMessage::onMessageDelAction()
{
    mChatWindow->deleteChatMessage(this->message);
}

void ChatMessage::onMessageEditAction()
{
    startEditingMsgWidget();
}

void ChatMessage::cancelMsgEdit(bool clicked)
{
    clearEdit();
    mChatWindow->ui->mMessageEdit->setText(QString());
}

void ChatMessage::saveMsgEdit(bool clicked)
{
    std::string editedMsg = mChatWindow->ui->mMessageEdit->toPlainText().toStdString();
    if(message->getContent() != editedMsg)
    {
        megaChatApi->editMessage(chatId,message->getMsgId(), editedMsg.c_str());
    }
    clearEdit();
}

void ChatMessage::startEditingMsgWidget()
{
    mChatWindow->ui->mMsgSendBtn->setEnabled(false);
    mChatWindow->ui->mMessageEdit->blockSignals(true);
    ui->mEditDisplay->hide();
    ui->mStatusDisplay->hide();

    QPushButton *cancelBtn = new QPushButton(this);
    connect(cancelBtn, SIGNAL(clicked(bool)), this, SLOT(cancelMsgEdit(bool)));
    cancelBtn->setText("Cancel edit");
    auto layout = static_cast<QBoxLayout*>(ui->mHeader->layout());
    layout->insertWidget(2, cancelBtn);

    QPushButton * saveBtn = new QPushButton(this);
    connect(saveBtn, SIGNAL(clicked(bool)), this, SLOT(saveMsgEdit(bool)));
    saveBtn->setText("Save");
    layout->insertWidget(3, saveBtn);

    setLayout(layout);
    mChatWindow->ui->mMessageEdit->setText(ui->mMsgDisplay->toPlainText());
    mChatWindow->ui->mMessageEdit->moveCursor(QTextCursor::End);
}

void ChatMessage::clearEdit()
{
    mChatWindow->ui->mMessageEdit->setText("");
    mChatWindow->ui->mMessageEdit->moveCursor(QTextCursor::Start);
    auto header = ui->mHeader->layout();
    auto cancelBtn = header->itemAt(2)->widget();
    auto saveBtn = header->itemAt(3)->widget();
    header->removeWidget(cancelBtn);
    header->removeWidget(saveBtn);
    ui->mEditDisplay->show();
    ui->mStatusDisplay->show();
    delete cancelBtn;
    delete saveBtn;
    mChatWindow->ui->mMsgSendBtn->setEnabled(true);
    mChatWindow->ui->mMessageEdit->blockSignals(false);
}

void ChatMessage::setManualMode(bool manualMode)
{
    if(manualMode)
    {
        ui->mEditDisplay->hide();
        ui->mStatusDisplay->hide();
        QPushButton * manualSendBtn = new QPushButton(this);
        connect(manualSendBtn, SIGNAL(clicked(bool)), this, SLOT(onManualSending()));
        manualSendBtn->setText("Send (Manual mode)");
        auto layout = static_cast<QBoxLayout*>(ui->mHeader->layout());
        layout->insertWidget(2, manualSendBtn);

        QPushButton * discardBtn = new QPushButton(this);
        connect(discardBtn, SIGNAL(clicked(bool)), this, SLOT(onDiscardManualSending()));
        discardBtn->setText("Discard");
        layout->insertWidget(3, discardBtn);
        setLayout(layout);
    }
    else
    {
        ui->mEditDisplay->show();
        ui->mStatusDisplay->show();
        auto header = ui->mHeader->layout();
        auto manualSending = header->itemAt(2)->widget();
        auto discardManualSending = header->itemAt(3)->widget();
        delete manualSending;
        delete discardManualSending;
    }
}

void ChatMessage::onManualSending()
{
   if(mChatWindow->mChatRoom->getOwnPrivilege() == megachat::MegaChatPeerList::PRIV_MODERATOR)
   {
       megaChatApi->removeUnsentMessage(mChatWindow->mChatRoom->getChatId(), message->getRowId());
       megachat::MegaChatMessage *tempMessage = megaChatApi->sendMessage(mChatWindow->mChatRoom->getChatId(), message->getContent());
       setManualMode(false);
       mChatWindow->eraseChatMessage(message, true);
       mChatWindow->moveManualSendingToSending(tempMessage);
   }
   else
   {
       QMessageBox::critical(nullptr, tr("Manual sending"), tr("You don't have permissions to send this message"));
   }
}

void ChatMessage::onDiscardManualSending()
{
   megaChatApi->removeUnsentMessage(mChatWindow->mChatRoom->getChatId(), message->getRowId());
   mChatWindow->eraseChatMessage(message, true);
}



