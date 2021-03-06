#include "gmailnotify.h"

#include <QDir>
#include <QMouseEvent>
#include <QApplication>
#include <QTextDocument>
#include <definitions/stanzahandlerorders.h>

#define SHC_GMAILNOTIFY                  "/iq[@type='set']/new-mail[@xmlns='" NS_GMAILNOTIFY "']"

#define NOTIFY_TIMEOUT                   30000
#define MAX_SEPARATE_NOTIFIES            3

GmailNotify::GmailNotify()
{
	FDiscovery = NULL;
	FXmppStreamManager= NULL;
	FStanzaProcessor = NULL;
	FNotifications = NULL;
	FRostersViewPlugin = NULL;

	FGmailLabelId = -1;

#ifdef DEBUG_RESOURCES_DIR
	FileStorage::setResourcesDirs(FileStorage::resourcesDirs() << DEBUG_RESOURCES_DIR);
#endif
}

GmailNotify::~GmailNotify()
{

}

void GmailNotify::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("GMail Notifications");
	APluginInfo->description = tr("Notify of new e-mails in Google Mail");
	APluginInfo->version = "1.0.5";
	APluginInfo->author = "Potapov S.A. aka Lion";
	APluginInfo->homePage = "http://code.google.com/p/vacuum-plugins";
	APluginInfo->dependences.append(STANZAPROCESSOR_UUID);
}

bool GmailNotify::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	IPlugin *plugin = APluginManager->pluginInterface("IStanzaProcessor").value(0);
	if (plugin)
	{
		FStanzaProcessor = qobject_cast<IStanzaProcessor *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IServiceDiscovery").value(0);
	if (plugin)
	{
		FDiscovery = qobject_cast<IServiceDiscovery *>(plugin->instance());
		if (FDiscovery)
		{
			connect(FDiscovery->instance(),SIGNAL(discoInfoReceived(const IDiscoInfo &)),SLOT(onDiscoveryInfoReceived(const IDiscoInfo &)));
		}
	}

	plugin = APluginManager->pluginInterface("IXmppStreamManager").value(0);
	if (plugin)
	{
		FXmppStreamManager = qobject_cast<IXmppStreamManager *>(plugin->instance());
		if (FXmppStreamManager)
		{
			connect(FXmppStreamManager->instance(),SIGNAL(streamOpened(IXmppStream *)),SLOT(onXmppStreamOpened(IXmppStream *)));
			connect(FXmppStreamManager->instance(),SIGNAL(streamClosed(IXmppStream *)),SLOT(onXmppStreamClosed(IXmppStream *)));
		}
	}

	plugin = APluginManager->pluginInterface("INotifications").value(0);
	if (plugin)
	{
		FNotifications = qobject_cast<INotifications *>(plugin->instance());
		if (FNotifications)
		{
			connect(FNotifications->instance(),SIGNAL(notificationActivated(int)),SLOT(onNotificationActivated(int)));
			connect(FNotifications->instance(),SIGNAL(notificationRemoved(int)),SLOT(onNotificationRemoved(int)));
		}
	}

	plugin = APluginManager->pluginInterface("IRostersViewPlugin").value(0);
	if (plugin)
	{
		FRostersViewPlugin = qobject_cast<IRostersViewPlugin *>(plugin->instance());
		if (FRostersViewPlugin)
		{
			connect(FRostersViewPlugin->rostersView()->instance(),SIGNAL(indexToolTips(IRosterIndex *, quint32, QMap<int,QString> &)),
				SLOT(onRostersViewIndexToolTips(IRosterIndex *, quint32, QMap<int,QString> &)));
		}
	}

	return FStanzaProcessor!=NULL;
}

bool GmailNotify::initObjects()
{
	if (FDiscovery)
	{
		registerDiscoFeatures();
	}
	if (FNotifications)
	{
		INotificationType notifyType;
		notifyType.order = NTO_GMAIL_NOTIFY;
		notifyType.icon = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_GMAILNOTIFY_GMAIL);
		notifyType.title = tr("When receiving a new message in google mail");
		notifyType.kindMask = INotification::PopupWindow|INotification::TrayNotify|INotification::SoundPlay|INotification::AutoActivate;
		notifyType.kindDefs = INotification::PopupWindow|INotification::TrayNotify|INotification::SoundPlay;
		FNotifications->registerNotificationType(NNT_GMAIL_NOTIFY,notifyType);
	}
	if (FRostersViewPlugin)
	{
		AdvancedDelegateItem notifyLabel(RLID_GMAILNOTIFY);
		notifyLabel.d->kind = AdvancedDelegateItem::CustomData;
		notifyLabel.d->data = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_GMAILNOTIFY_GMAIL);
		FGmailLabelId = FRostersViewPlugin->rostersView()->registerLabel(notifyLabel);

		FRostersViewPlugin->rostersView()->insertClickHooker(RCHO_GMAILNOTIFY,this);
	}
	return true;
}

bool GmailNotify::stanzaReadWrite(int AHandleId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept)
{
	if (FSHIGmailNotify.value(AStreamJid)==AHandleId && AStanza.isFromServer())
	{
		AAccept = true;
		Stanza reply = FStanzaProcessor->makeReplyResult(AStanza);
		FStanzaProcessor->sendStanzaOut(AStreamJid,reply);
		checkNewMail(AStreamJid,true);
	}
	return false;
}

void GmailNotify::stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	if (FMailRequests.contains(AStanza.id()))
	{
		bool full = FMailRequests.take(AStanza.id());
		if (AStanza.type() == "result")
		{
			if (!isSupported(AStreamJid))
			{
				Stanza enable(STANZA_KIND_IQ);
				enable.setType(STANZA_TYPE_SET).setUniqueId();
				enable.addElement("usersetting",NS_GOOGLESETTINGS).appendChild(enable.createElement("mailnotifications")).toElement().setAttribute("value","true");
				FStanzaProcessor->sendStanzaOut(AStreamJid,enable);
				insertStanzaHandler(AStreamJid);
			}
			processGmailReply(AStreamJid,parseGmailReply(AStanza),full);
		}
	}
}

bool GmailNotify::rosterIndexSingleClicked(int AOrder, IRosterIndex *AIndex, const QMouseEvent *AEvent)
{
	if (AOrder == RCHO_GMAILNOTIFY)
	{
		QModelIndex index = FRostersViewPlugin->rostersView()->mapFromModel(FRostersViewPlugin->rostersView()->rostersModel()->modelIndexFromRosterIndex(AIndex));
		if (FRostersViewPlugin->rostersView()->labelAt(AEvent->pos(),index) == FGmailLabelId)
		{
			showNotifyDialog(AIndex->data(RDR_STREAM_JID).toString());
			return true;
		}
	}
	return false;
}

bool GmailNotify::rosterIndexDoubleClicked(int AOrder, IRosterIndex *AIndex, const QMouseEvent *AEvent)
{
	Q_UNUSED(AOrder); Q_UNUSED(AIndex); Q_UNUSED(AEvent);
	return false;
}

bool GmailNotify::isSupported(const Jid &AStreamJid) const
{
	return FSHIGmailNotify.contains(AStreamJid);
}

IGmailReply GmailNotify::gmailReply(const Jid &AAccountJid) const
{
	return FAccountReply.value(AAccountJid.pBare());
}

QDialog *GmailNotify::showNotifyDialog(const Jid &AAccountJid)
{
	QPointer<NotifyGmailDialog> dilog = FAccountDialog.value(AAccountJid.pBare());
	IGmailReply reply = FAccountReply.value(AAccountJid.pBare());
	if (reply.theads.count() > 0)
	{
		foreach(int notifyId, findAccountNotifies(AAccountJid))
			FNotifications->removeNotification(notifyId);

		if (dilog.isNull())
		{
			dilog = new NotifyGmailDialog(AAccountJid.pBare());
			FAccountDialog.insert(AAccountJid.pBare(),dilog);
		}
		dilog->setGmailReply(reply);
		dilog->adjustSize();
		WidgetManager::showActivateRaiseWindow(dilog);
	}
	return dilog;
}

bool GmailNotify::checkNewMail(const Jid &AStreamJid, bool AFull)
{
	Stanza query(STANZA_KIND_IQ);
	query.setType(STANZA_TYPE_GET).setUniqueId();
	QDomElement queryElem = query.addElement("query",NS_GMAILNOTIFY);
   
   if (!AFull)
   {
      QString lastTid = Options::node(OPV_GMAILNOTIFY_ACCOUNT_ITEM,AStreamJid.pBare()).value("last-tid").toString();
      if (!lastTid.isEmpty())
         queryElem.setAttribute("newer-than-tid",lastTid);

      QString lastTime = Options::node(OPV_GMAILNOTIFY_ACCOUNT_ITEM,AStreamJid.pBare()).value("last-time").toString();
      if (!lastTime.isEmpty())
         queryElem.setAttribute("newer-than-time",lastTime);
   }

	if (FStanzaProcessor->sendStanzaRequest(this,AStreamJid,query,NOTIFY_TIMEOUT))
	{
		FMailRequests.insert(query.id(),AFull);
		return true;
	}
	return false;
}

void GmailNotify::setGmailReply(const Jid &AStreamJid, const IGmailReply &AReply)
{
	if (FRostersViewPlugin && FRostersViewPlugin->rostersView()->rostersModel())
	{
		IRosterIndex *stream = FRostersViewPlugin->rostersView()->rostersModel()->streamIndex(AStreamJid);
		if (stream)
		{
			if (AReply.theads.count() > 0)
				FRostersViewPlugin->rostersView()->insertLabel(FGmailLabelId,stream);
			else
				FRostersViewPlugin->rostersView()->removeLabel(FGmailLabelId,stream);
		}
	}
	if (!AReply.resultTime.isEmpty())
		FAccountReply.insert(AStreamJid.pBare(),AReply);
	else
		FAccountReply.remove(AStreamJid.pBare());
	emit gmailReplyChanged(AStreamJid.pBare(),AReply);
}

IGmailReply GmailNotify::parseGmailReply(const Stanza &AStanza) const
{
	IGmailReply reply;
	reply.totalMatched = 0;
	reply.totalEstimate = 0;

	QDomElement replyElem = AStanza.firstElement("mailbox",NS_GMAILNOTIFY);
	if (!replyElem.isNull())
	{
		reply.resultTime = replyElem.attribute("result-time");
		reply.totalMatched = replyElem.attribute("total-matched").toInt();
		reply.totalEstimate = replyElem.attribute("total-estimate").toInt();
		reply.mailUrl = replyElem.attribute("url");

		QDomElement theadElem = replyElem.firstChildElement("mail-thread-info");
		while (!theadElem.isNull())
		{
			IGmailThread gthread;
			gthread.threadId = theadElem.attribute("tid");
			gthread.participation = theadElem.attribute("participation").toInt();
			gthread.messages = theadElem.attribute("messages").toInt();
			gthread.timestamp = theadElem.attribute("date").toLongLong();
			gthread.threadUrl = theadElem.attribute("url");
			gthread.labels = theadElem.firstChildElement("labels").text();
			gthread.subject = theadElem.firstChildElement("subject").text();
			if (gthread.subject.isEmpty())
				gthread.subject = tr("<no subject>");
			gthread.snippet = theadElem.firstChildElement("snippet").text();

			QDomElement senderElem = theadElem.firstChildElement("senders").firstChildElement("sender");
			while (!senderElem.isNull())
			{
				IGmailSender sender;
				sender.name = senderElem.attribute("name");
				sender.address = senderElem.attribute("address");
				sender.originator = senderElem.attribute("originator").toInt() == 1;
				sender.unread = senderElem.attribute("unread").toInt() == 1;

				gthread.senders.append(sender);
				senderElem = senderElem.nextSiblingElement("sender");
			}

			reply.theads.append(gthread);
			theadElem = theadElem.nextSiblingElement("mail-thread-info");
		}
	}

	return reply;
}

QList<int> GmailNotify::findAccountNotifies(const Jid &AAccountJid) const
{
	QList<int> notifies;
	for (QMap<int,Jid>::const_iterator it = FNotifies.constBegin(); it!=FNotifies.constEnd(); ++it)
	{
		if (AAccountJid.pBare() == it.value().pBare())
			notifies.append(it.key());
	}
	return notifies;
}

int GmailNotify::findThreadNotify(const Jid &AAccountJid, const QString &AThreadId) const
{
	for (QMap<int,Jid>::const_iterator it = FNotifies.constBegin(); it!=FNotifies.constEnd(); ++it)
	{
		if (AAccountJid.pBare()==it.value().pBare() && it.value().resource()==AThreadId)
			return it.key();
	}
	return -1;
}

void GmailNotify::processGmailReply(const Jid &AStreamJid, const IGmailReply &AReply, bool AFull)
{
	if (FNotifications && !AReply.resultTime.isEmpty())
	{
		if (AFull)
		{
			QPointer<NotifyGmailDialog> dilog = FAccountDialog.value(AStreamJid.pBare());
			if (dilog.isNull())
			{
				if (FAccountReply.contains(AStreamJid.pBare()))
				{
					QList<QString> beforeThreads;
					IGmailReply beforeReply = FAccountReply.value(AStreamJid.pBare());
					foreach(const IGmailThread &gthread, beforeReply.theads) 
						beforeThreads.append(gthread.threadId);

					QList<QString> afterThreads;
					foreach(const IGmailThread &gthread, AReply.theads)
						afterThreads.append(gthread.threadId);

					foreach(QString threadId, beforeThreads.toSet()-afterThreads.toSet())
						FNotifications->removeNotification(findThreadNotify(AStreamJid,threadId));
					FNotifications->removeNotification(findThreadNotify(AStreamJid,QString::null));

					QList<IGmailThread> notifyThreads;
					foreach(QString threadId,afterThreads.toSet())
					{
						bool notify = true;
						IGmailThread afterThread = AReply.theads.value(afterThreads.indexOf(threadId));
						if (beforeThreads.contains(threadId))
						{
							IGmailThread beforeThread = beforeReply.theads.value(beforeThreads.indexOf(threadId));
							notify = afterThread.timestamp > beforeThread.timestamp;
						}
						if (notify)
						{
							notifyThreads.append(afterThread);
						}
					}
					notifyGmailThreads(AStreamJid,notifyThreads,false);
				}
				else
				{
					notifyGmailThreads(AStreamJid,AReply.theads,true);
				}
			}
			else if (!AReply.theads.isEmpty())
			{
				dilog->setGmailReply(AReply);
				dilog->adjustSize();
			}
			else
			{
				dilog->reject();
			}
			setGmailReply(AStreamJid,AReply);
		}
		else
		{
			notifyGmailThreads(AStreamJid,AReply.theads,false);
		}

		if (!AReply.theads.isEmpty())
			Options::node(OPV_GMAILNOTIFY_ACCOUNT_ITEM,AStreamJid.pBare()).setValue(AReply.theads.value(0).threadId,"last-tid");
		Options::node(OPV_GMAILNOTIFY_ACCOUNT_ITEM,AStreamJid.pBare()).setValue(AReply.resultTime,"last-time");
	}
}

void GmailNotify::notifyGmailThreads(const Jid &AStreamJid, const QList<IGmailThread> &AThreads, bool ATotal)
{
	if (FNotifications && AThreads.count()>0)
	{
		INotification notify;
		notify.kinds = FNotifications->enabledTypeNotificationKinds(NNT_GMAIL_NOTIFY);
		notify.typeId = NNT_GMAIL_NOTIFY;
		if (notify.kinds > 0)
		{
			Jid notifyJid = AStreamJid.bare();
			notify.data.insert(NDR_ICON,IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_GMAILNOTIFY_GMAIL));
			notify.data.insert(NDR_TOOLTIP,tr("New e-mail for %1").arg(AStreamJid.uBare()));
			if (ATotal || AThreads.count()>MAX_SEPARATE_NOTIFIES)
			{
				notify.data.insert(NDR_POPUP_CAPTION,tr("New e-mail"));
				notify.data.insert(NDR_POPUP_TITLE,AStreamJid.uBare());
				if (ATotal)
					notify.data.insert(NDR_POPUP_TEXT,tr("You have %n unread message(s)","",AThreads.count()));
				else
					notify.data.insert(NDR_POPUP_TEXT,tr("You have %n new unread message(s)","",AThreads.count()));
				FNotifies.insert(FNotifications->appendNotification(notify),notifyJid);
			}
			else for (int i=0; i<AThreads.count(); i++)
			{
				IGmailThread gthread = AThreads.value(i);
				IGmailSender sender = gthread.senders.value(0);
				notifyJid.setResource(gthread.threadId);
				notify.data.insert(NDR_POPUP_CAPTION,tr("New e-mail for %1").arg(AStreamJid.uBare()));
				notify.data.insert(NDR_POPUP_TITLE,!sender.name.isEmpty() ? QString("%1 <%2>").arg(sender.name).arg(sender.address) : sender.address);
				notify.data.insert(NDR_POPUP_TEXT,gthread.subject + " - " + gthread.snippet);
				FNotifies.insert(FNotifications->appendNotification(notify),notifyJid);
			}
		}
	}
}

void GmailNotify::registerDiscoFeatures()
{
	IDiscoFeature dfeature;
	dfeature.var = NS_GMAILNOTIFY;
	dfeature.active = false;
	dfeature.name = tr("GMail Notifications");
	dfeature.description = tr("Supports the notifications of new e-mails in Google Mail");
	FDiscovery->insertDiscoFeature(dfeature);
}

void GmailNotify::insertStanzaHandler(const Jid &AStreamJid)
{
	if (FStanzaProcessor && !FSHIGmailNotify.contains(AStreamJid))
	{
		IStanzaHandle shandle;
		shandle.handler = this;
		shandle.streamJid = AStreamJid;
		shandle.direction = IStanzaHandle::DirectionIn;
		shandle.order = SHO_DEFAULT;
		shandle.conditions.append(SHC_GMAILNOTIFY);
		FSHIGmailNotify.insert(AStreamJid,FStanzaProcessor->insertStanzaHandle(shandle));
	}
}

void GmailNotify::removeStanzaHandler(const Jid &AStreamJid)
{
	if (FStanzaProcessor)
		FStanzaProcessor->removeStanzaHandle(FSHIGmailNotify.take(AStreamJid));
}

void GmailNotify::onXmppStreamOpened(IXmppStream *AXmppStream)
{
	foreach(int notifyId, findAccountNotifies(AXmppStream->streamJid()))
		FNotifications->removeNotification(notifyId);
	setGmailReply(AXmppStream->streamJid(),IGmailReply());

	if (FDiscovery == NULL)
		checkNewMail(AXmppStream->streamJid(),true);
}

void GmailNotify::onXmppStreamClosed(IXmppStream *AXmppStream)
{
	removeStanzaHandler(AXmppStream->streamJid());
}

void GmailNotify::onDiscoveryInfoReceived(const IDiscoInfo &AInfo)
{
	if (AInfo.contactJid==AInfo.streamJid.domain() && AInfo.node.isEmpty())
	{
		if (!isSupported(AInfo.streamJid) && AInfo.features.contains(NS_GMAILNOTIFY))
			checkNewMail(AInfo.streamJid,true);
	}
}

void GmailNotify::onNotificationActivated(int ANotifyId)
{
	if (FNotifies.contains(ANotifyId))
		showNotifyDialog(FNotifies.value(ANotifyId));
}

void GmailNotify::onNotificationRemoved(int ANotifyId)
{
	if (FNotifies.contains(ANotifyId))
		FNotifies.remove(ANotifyId);
}

void GmailNotify::onRostersViewIndexToolTips(IRosterIndex *AIndex, quint32 ALabelId, QMap<int,QString> &AToolTips)
{
	if (ALabelId == FGmailLabelId)
	{
		IGmailReply reply = gmailReply(AIndex->data(RDR_STREAM_JID).toString());
		if (reply.theads.count() > 0)
		{
			QString tooltip = tr("You have <b>%n unread letter(s)</b>:","",reply.totalMatched);
			tooltip += "<ul>";
			foreach(const IGmailThread &gthread, reply.theads)
				tooltip += QString("<li>%1 (%2)</li>").arg(gthread.subject.toHtmlEscaped()).arg(gthread.messages);
			tooltip += "</ul>";
			AToolTips.insert(RTTO_GMAILNOTIFY,tooltip);
		}
	}
}
