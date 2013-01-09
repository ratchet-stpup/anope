/* MemoServ core functions
 *
 * (C) 2003-2013 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 */

/*************************************************************************/

#include "module.h"
#include "memoserv.h"

static bool SendMemoMail(NickCore *nc, MemoInfo *mi, Memo *m)
{
	Anope::string subject = Language::Translate(nc, Config->MailMemoSubject.c_str());
	Anope::string message = Language::Translate(nc, Config->MailMemoMessage.c_str());

	subject = subject.replace_all_cs("%n", nc->display);
	subject = subject.replace_all_cs("%s", m->sender);
	subject = subject.replace_all_cs("%d", stringify(mi->GetIndex(m) + 1));
	subject = subject.replace_all_cs("%t", m->text);
	subject = subject.replace_all_cs("%N", Config->NetworkName);

	message = message.replace_all_cs("%n", nc->display);
	message = message.replace_all_cs("%s", m->sender);
	message = message.replace_all_cs("%d", stringify(mi->GetIndex(m) + 1));
	message = message.replace_all_cs("%t", m->text);
	message = message.replace_all_cs("%N", Config->NetworkName);

	return Mail::Send(nc, subject, message);
}

class MyMemoServService : public MemoServService
{
 public:
	MyMemoServService(Module *m) : MemoServService(m) { }

 	MemoInfo *GetMemoInfo(const Anope::string &target, bool &ischan) anope_override
	{
		if (!target.empty() && target[0] == '#')
		{
			ischan = true;
			ChannelInfo *ci = ChannelInfo::Find(target);
			if (ci != NULL)
				return &ci->memos;
		}
		else
		{
			ischan = false;
			NickAlias *na = NickAlias::Find(target);
			if (na != NULL)
				return &na->nc->memos;
		}

		return NULL;
	}

	MemoResult Send(const Anope::string &source, const Anope::string &target, const Anope::string &message, bool force) anope_override
	{
		bool ischan;
		MemoInfo *mi = this->GetMemoInfo(target, ischan);

		if (mi == NULL)
			return MEMO_INVALID_TARGET;

		User *sender = User::Find(source);
		if (sender != NULL && !sender->HasPriv("memoserv/no-limit") && !force)
		{
			if (Config->MSSendDelay > 0 && sender->lastmemosend + Config->MSSendDelay > Anope::CurTime)
				return MEMO_TOO_FAST;
			else if (!mi->memomax)
				return MEMO_TARGET_FULL;
			else if (mi->memomax > 0 && mi->memos->size() >= static_cast<unsigned>(mi->memomax))
				return MEMO_TARGET_FULL;
			else if (mi->HasIgnore(sender))
				return MEMO_SUCCESS;
		}

		if (sender != NULL)
			sender->lastmemosend = Anope::CurTime;

		Memo *m = new Memo();
		mi->memos->push_back(m);
		m->owner = target;
		m->sender = source;
		m->time = Anope::CurTime;
		m->text = message;
		m->SetFlag(MF_UNREAD);

		FOREACH_MOD(I_OnMemoSend, OnMemoSend(source, target, mi, m));

		if (ischan)
		{
			ChannelInfo *ci = ChannelInfo::Find(target);

			if (ci->c)
			{
				for (Channel::ChanUserList::iterator it = ci->c->users.begin(), it_end = ci->c->users.end(); it != it_end; ++it)
				{
					ChanUserContainer *cu = *it;

					if (ci->AccessFor(cu->user).HasPriv("MEMO"))
					{
						if (cu->user->Account() && cu->user->Account()->HasFlag(NI_MEMO_RECEIVE))
							cu->user->SendMessage(MemoServ, MEMO_NEW_X_MEMO_ARRIVED, ci->name.c_str(), Config->UseStrictPrivMsgString.c_str(), Config->MemoServ.c_str(), ci->name.c_str(), mi->memos->size());
					}
				}
			}
		}
		else
		{
			NickCore *nc = NickAlias::Find(target)->nc;

			if (nc->HasFlag(NI_MEMO_RECEIVE))
			{
				for (std::list<Serialize::Reference<NickAlias> >::const_iterator it = nc->aliases.begin(), it_end = nc->aliases.end(); it != it_end;)
				{
					const NickAlias *na = *it++;
					if (!na)
						continue;
					User *user = User::Find(na->nick);
					if (user && user->IsIdentified())
						user->SendMessage(MemoServ, MEMO_NEW_MEMO_ARRIVED, source.c_str(), Config->UseStrictPrivMsgString.c_str(), Config->MemoServ.c_str(), mi->memos->size());
				}
			}

			/* let's get out the mail if set in the nickcore - certus */
			if (nc->HasFlag(NI_MEMO_MAIL))
				SendMemoMail(nc, mi, m);
		}

		return MEMO_SUCCESS;
	}

	void Check(User *u)
	{
		const NickCore *nc = u->Account();
		if (!nc)
			return;

		unsigned i = 0, end = nc->memos.memos->size(), newcnt = 0;
		for (; i < end; ++i)
			if (nc->memos.GetMemo(i)->HasFlag(MF_UNREAD))
				++newcnt;
		if (newcnt > 0)
			u->SendMessage(MemoServ, newcnt == 1 ? _("You have 1 new memo.") : _("You have %d new memos."), newcnt);
		if (nc->memos.memomax > 0 && nc->memos.memos->size() >= static_cast<unsigned>(nc->memos.memomax))
		{
			if (nc->memos.memos->size() > static_cast<unsigned>(nc->memos.memomax))
				u->SendMessage(MemoServ, _("You are over your maximum number of memos (%d). You will be unable to receive any new memos until you delete some of your current ones."), nc->memos.memomax);
			else
				u->SendMessage(MemoServ, _("You have reached your maximum number of memos (%d). You will be unable to receive any new memos until you delete some of your current ones."), nc->memos.memomax);
		}
	}
};

class MemoServCore : public Module
{
	MyMemoServService mymemoserv;
 public:
	MemoServCore(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator, CORE),
		mymemoserv(this)
	{
		this->SetAuthor("Anope");

		MemoServ = BotInfo::Find(Config->MemoServ);
		if (!MemoServ)
			throw ModuleException("No bot named " + Config->MemoServ);

		Implementation i[] = { I_OnBotDelete, I_OnNickIdentify, I_OnJoinChannel, I_OnUserAway, I_OnNickUpdate, I_OnPreHelp, I_OnPostHelp };
		ModuleManager::Attach(i, this, sizeof(i) / sizeof(Implementation));
	}

	~MemoServCore()
	{
		MemoServ = NULL;
	}

	void OnBotDelete(BotInfo *bi) anope_override
	{
		if (bi == MemoServ)
			MemoServ = NULL;
	}

	void OnNickIdentify(User *u) anope_override
	{
		this->mymemoserv.Check(u);
	}

	void OnJoinChannel(User *u, Channel *c) anope_override
	{
		if (c->ci && c->ci->AccessFor(u).HasPriv("MEMO") && c->ci->memos.memos->size() > 0)
		{
			if (c->ci->memos.memos->size() == 1)
				u->SendMessage(MemoServ, _("There is \002%d\002 memo on channel %s."), c->ci->memos.memos->size(), c->ci->name.c_str());
			else
				u->SendMessage(MemoServ, _("There are \002%d\002 memos on channel %s."), c->ci->memos.memos->size(), c->ci->name.c_str());
		}
	}

	void OnUserAway(User *u, const Anope::string &message) anope_override
	{
		if (message.empty())
			this->mymemoserv.Check(u);
	}

	void OnNickUpdate(User *u) anope_override
	{
		this->mymemoserv.Check(u);
	}

	EventReturn OnPreHelp(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{
		if (!params.empty() || source.c || source.service->nick != Config->MemoServ)
			return EVENT_CONTINUE;
		source.Reply(_("\002%s\002 is a utility allowing IRC users to send short\n"
			"messages to other IRC users, whether they are online at\n"
			"the time or not, or to channels(*). Both the sender's\n"
			"nickname and the target nickname or channel must be\n"
			"registered in order to send a memo.\n"
			"%s's commands include:"), Config->MemoServ.c_str(), Config->MemoServ.c_str());
		return EVENT_CONTINUE;
	}

	void OnPostHelp(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{
		if (!params.empty() || source.c || source.service->nick != Config->MemoServ)
			return;
		source.Reply(_(" \n"
			"Type \002%s%s HELP \037command\037\002 for help on any of the\n"
			"above commands.\n"
			"(*) By default, any user with at least level 10 access on a\n"
			"    channel can read that channel's memos. This can be\n"
			"    changed with the %s \002LEVELS\002 command."), Config->UseStrictPrivMsgString.c_str(), Config->MemoServ.c_str(), Config->ChanServ.c_str());
	}
};

MODULE_INIT(MemoServCore)

