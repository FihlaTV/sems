/*
 * Copyright (C) 2010 Stefan Sayer
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* 
SBC - feature-wishlist
- accounting (MySQL DB, cassandra DB)
- RTP transcoding mode (bridging)
- overload handling (parallel call to target thresholds)
- call distribution
- select profile on monitoring in-mem DB record
- fallback profile
 */
#include "SBC.h"
#include "ampi/SBCCallControlAPI.h"

#include "log.h"
#include "AmUtils.h"
#include "AmAudio.h"
#include "AmPlugIn.h"
#include "AmMediaProcessor.h"
#include "AmConfigReader.h"
#include "AmSessionContainer.h"
#include "AmSipHeaders.h"

#include "HeaderFilter.h"
#include "ParamReplacer.h"
#include "SDPFilter.h"
#include "SBCCallRegistry.h"
#include "ReplacesMapper.h"

using std::map;

AmConfigReader SBCFactory::cfg;
AmSessionEventHandlerFactory* SBCFactory::session_timer_fact = NULL;
RegexMapper SBCFactory::regex_mappings;

EXPORT_MODULE_FACTORY(SBCFactory);
DEFINE_MODULE_INSTANCE(SBCFactory, MOD_NAME);

SBCFactory::SBCFactory(const string& _app_name)
  : AmSessionFactory(_app_name), AmDynInvokeFactory(_app_name)
{
}

SBCFactory::~SBCFactory() {
}

int SBCFactory::onLoad()
{
  if(cfg.loadFile(AmConfig::ModConfigPath + string(MOD_NAME ".conf"))) {
    ERROR("No configuration for sbc present (%s)\n",
	 (AmConfig::ModConfigPath + string(MOD_NAME ".conf")).c_str()
	 );
    return -1;
  }

  session_timer_fact = AmPlugIn::instance()->getFactory4Seh("session_timer");
  if(!session_timer_fact) {
    ERROR("could not load session_timer from session_timer plug-in\n");
    return -1;
  }

  vector<string> profiles_names = explode(cfg.getParameter("profiles"), ",");
  for (vector<string>::iterator it =
	 profiles_names.begin(); it != profiles_names.end(); it++) {
    string profile_file_name = AmConfig::ModConfigPath + *it + ".sbcprofile.conf";
    if (!call_profiles[*it].readFromConfiguration(*it, profile_file_name)) {
      ERROR("configuring SBC call profile from '%s'\n", profile_file_name.c_str());
      return -1;
    }
  }

  active_profile = explode(cfg.getParameter("active_profile"), ",");
  if (active_profile.empty()) {
    ERROR("active_profile not set.\n");
    return -1;
  }

  string active_profile_s;
  for (vector<string>::iterator it =
	 active_profile.begin(); it != active_profile.end(); it++) {
    if (it->empty())
      continue;
    if (((*it)[0] != '$') && call_profiles.find(*it) == call_profiles.end()) {
      ERROR("call profile active_profile '%s' not loaded!\n", it->c_str());
      return -1;
    }
    active_profile_s+=*it;
    if (it != active_profile.end()-1)
      active_profile_s+=", ";
  }

  INFO("SBC: active profile: '%s'\n", active_profile_s.c_str());

  vector<string> regex_maps = explode(cfg.getParameter("regex_maps"), ",");
  for (vector<string>::iterator it =
	 regex_maps.begin(); it != regex_maps.end(); it++) {
    string regex_map_file_name = AmConfig::ModConfigPath + *it + ".conf";
    RegexMappingVector v;
    if (!read_regex_mapping(regex_map_file_name, "=>",
			    ("SBC regex mapping " + *it+":").c_str(), v)) {
      ERROR("reading regex mapping from '%s'\n", regex_map_file_name.c_str());
      return -1;
    }
    regex_mappings.setRegexMap(*it, v);
    INFO("loaded regex mapping '%s'\n", it->c_str());
  }

  if (!AmPlugIn::registerApplication(MOD_NAME, this)) {
    ERROR("registering "MOD_NAME" application\n");
    return -1;
  }

  if (!AmPlugIn::registerDIInterface(MOD_NAME, this)) {
    ERROR("registering "MOD_NAME" DI interface\n");
    return -1;
  }

  return 0;
}

#define REPLACE_VALS req, app_param, ruri_parser, from_parser, to_parser

/** get the first matching profile name from active profiles */
string SBCFactory::getActiveProfileMatch(string& profile_rule, const AmSipRequest& req,
					 const string& app_param, AmUriParser& ruri_parser,
					 AmUriParser& from_parser, AmUriParser& to_parser) {
  string res;
  for (vector<string>::iterator it=
	 active_profile.begin(); it != active_profile.end(); it++) {
    if (it->empty())
      continue;

    if (*it == "$(paramhdr)")
      res = get_header_keyvalue(app_param,"profile");
    else if (*it == "$(ruri.user)")
      res = req.user;
    else
      res = replaceParameters(*it, "active_profile", REPLACE_VALS);

    if (!res.empty()) {
      profile_rule = *it;    
      break;
    }
  }
  return res;
}

AmSession* SBCFactory::onInvite(const AmSipRequest& req)
{
  AmUriParser ruri_parser, from_parser, to_parser;

  profiles_mut.lock();

  string app_param = getHeader(req.hdrs, PARAM_HDR, true);
  
  string profile_rule;
  string profile = getActiveProfileMatch(profile_rule, REPLACE_VALS);

  map<string, SBCCallProfile>::iterator it=
    call_profiles.find(profile);
  if (it==call_profiles.end()) {
    profiles_mut.unlock();
    ERROR("could not find call profile '%s' (matching active_profile rule: '%s')\n",
	  profile.c_str(), profile_rule.c_str());
    throw AmSession::Exception(500,SIP_REPLY_SERVER_INTERNAL_ERROR);
  }

  DBG("using call profile '%s' (from matching active_profile rule '%s')\n",
      profile.c_str(), profile_rule.c_str());
  SBCCallProfile& call_profile = it->second;

  if (!call_profile.refuse_with.empty()) {
    string refuse_with = replaceParameters(call_profile.refuse_with,
					   "refuse_with", REPLACE_VALS);

    if (refuse_with.empty()) {
      ERROR("refuse_with empty after replacing (was '%s' in profile %s)\n",
	    call_profile.refuse_with.c_str(), call_profile.profile_file.c_str());
      profiles_mut.unlock();
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }

    size_t spos = refuse_with.find(' ');
    unsigned int refuse_with_code;
    if (spos == string::npos || spos == refuse_with.size() ||
	str2i(refuse_with.substr(0, spos), refuse_with_code)) {
      ERROR("invalid refuse_with '%s'->'%s' in  %s. Expected <code> <reason>\n",
	    call_profile.refuse_with.c_str(), refuse_with.c_str(),
	    call_profile.profile_file.c_str());
      profiles_mut.unlock();
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
    string refuse_with_reason = refuse_with.substr(spos+1);

    string hdrs = replaceParameters(call_profile.append_headers,
				    "append_headers", REPLACE_VALS);
    profiles_mut.unlock();

    if (hdrs.size()>2)
      assertEndCRLF(hdrs);

    DBG("refusing call with %u %s\n", refuse_with_code, refuse_with_reason.c_str());
    AmSipDialog::reply_error(req, refuse_with_code, refuse_with_reason, hdrs);

    return NULL;
  }

  AmConfigReader& sst_cfg = call_profile.use_global_sst_config ?
    cfg : call_profile.cfg; // override with profile config

  if (call_profile.sst_enabled) {
    DBG("Enabling SIP Session Timers\n");
    try {
      if (!session_timer_fact->onInvite(req, sst_cfg)) {
	profiles_mut.unlock();
	return NULL;
      }
    } catch (const AmSession::Exception& e) {
      profiles_mut.unlock();
      throw;
    }
  }

  SBCDialog* b2b_dlg = new SBCDialog(call_profile);

  if (call_profile.sst_enabled) {
    AmSessionEventHandler* h = session_timer_fact->getHandler(b2b_dlg);
    if(!h) {
      profiles_mut.unlock();
      delete b2b_dlg;
      ERROR("could not get a session timer event handler\n");
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }

    if (h->configure(sst_cfg)){
      ERROR("Could not configure the session timer: disabling session timers.\n");
      delete h;
    } else {
      b2b_dlg->addHandler(h);
    }
  }
  profiles_mut.unlock();

  return b2b_dlg;
}

void SBCFactory::invoke(const string& method, const AmArg& args, 
				AmArg& ret)
{
  if (method == "listProfiles"){
    listProfiles(args, ret);
  } else if (method == "reloadProfiles"){
    reloadProfiles(args,ret);
  } else if (method == "loadProfile"){
    args.assertArrayFmt("u");
    loadProfile(args,ret);
  } else if (method == "reloadProfile"){
    args.assertArrayFmt("u");
    reloadProfile(args,ret);
  } else if (method == "getActiveProfile"){
    getActiveProfile(args,ret);
  } else if (method == "setActiveProfile"){
    args.assertArrayFmt("u");
    setActiveProfile(args,ret);
  } else if (method == "getRegexMapNames"){
    getRegexMapNames(args,ret);
  } else if (method == "setRegexMap"){
    args.assertArrayFmt("u");
    setRegexMap(args,ret);
  } else if (method == "postControlCmd"){
    args.assertArrayFmt("ss"); // at least call-ltag, cmd
    postControlCmd(args,ret);
  } else if(method == "_list"){ 
    ret.push(AmArg("listProfiles"));
    ret.push(AmArg("reloadProfiles"));
    ret.push(AmArg("reloadProfile"));
    ret.push(AmArg("loadProfile"));
    ret.push(AmArg("getActiveProfile"));
    ret.push(AmArg("setActiveProfile"));
    ret.push(AmArg("getRegexMapNames"));
    ret.push(AmArg("setRegexMap"));
    ret.push(AmArg("postControlCmd"));
  }  else
    throw AmDynInvoke::NotImplemented(method);
}

void SBCFactory::listProfiles(const AmArg& args, AmArg& ret) {
  profiles_mut.lock();
  for (std::map<string, SBCCallProfile>::iterator it=
	 call_profiles.begin(); it != call_profiles.end(); it++) {
    AmArg p;
    p["name"] = it->first;
    p["md5"] = it->second.md5hash;
    p["path"] = it->second.profile_file;
    ret.push((p));
  }
  profiles_mut.unlock();
}

void SBCFactory::reloadProfiles(const AmArg& args, AmArg& ret) {
  std::map<string, SBCCallProfile> new_call_profiles;
  
  bool failed = false;
  string res = "OK";
  AmArg profile_list;
  profiles_mut.lock();
  for (std::map<string, SBCCallProfile>::iterator it=
	 call_profiles.begin(); it != call_profiles.end(); it++) {
    new_call_profiles[it->first] = SBCCallProfile();
    if (!new_call_profiles[it->first].readFromConfiguration(it->first,
							    it->second.profile_file)) {
      ERROR("reading call profile file '%s'\n", it->second.profile_file.c_str());
      res = "Error reading call profile for "+it->first+" from "+it->second.profile_file+
	+"; no profiles reloaded";
      failed = true;
      break;
    }
    AmArg p;
    p["name"] = it->first;
    p["md5"] = it->second.md5hash;
    p["path"] = it->second.profile_file;
    profile_list.push(p);
  }
  if (!failed) {
    call_profiles = new_call_profiles;
    ret.push(200);
  } else {
    ret.push(500);
  }
  ret.push(res);
  ret.push(profile_list);
  profiles_mut.unlock();
}

void SBCFactory::reloadProfile(const AmArg& args, AmArg& ret) {
  bool failed = false;
  string res = "OK";
  AmArg p;
  if (!args[0].hasMember("name")) {
    ret.push(400);
    ret.push("Parameters error: expected ['name': profile_name] ");
    return;
  }

  profiles_mut.lock();
  std::map<string, SBCCallProfile>::iterator it=
    call_profiles.find(args[0]["name"].asCStr());
  if (it == call_profiles.end()) {
    res = "profile '"+string(args[0]["name"].asCStr())+"' not found";
    failed = true;
  } else {
    SBCCallProfile new_cp;
    if (!new_cp.readFromConfiguration(it->first, it->second.profile_file)) {
      ERROR("reading call profile file '%s'\n", it->second.profile_file.c_str());
      res = "Error reading call profile for "+it->first+" from "+it->second.profile_file;
      failed = true;
    } else {
      it->second = new_cp;
      p["name"] = it->first;
      p["md5"] = it->second.md5hash;
      p["path"] = it->second.profile_file;
    }
  }
  profiles_mut.unlock();

  if (!failed) {
    ret.push(200);
    ret.push(res);
    ret.push(p);
  } else {
    ret.push(500);
    ret.push(res);
  }
}

void SBCFactory::loadProfile(const AmArg& args, AmArg& ret) {
  if (!args[0].hasMember("name") || !args[0].hasMember("path")) {
    ret.push(400);
    ret.push("Parameters error: expected ['name': profile_name] "
	     "and ['path': profile_path]");
    return;
  }
  SBCCallProfile cp;
  if (!cp.readFromConfiguration(args[0]["name"].asCStr(), args[0]["path"].asCStr())) {
    ret.push(500);
    ret.push("Error reading sbc call profile for "+string(args[0]["name"].asCStr())+
	     " from file "+string(args[0]["path"].asCStr()));
    return;
  }

  profiles_mut.lock();
  call_profiles[args[0]["name"].asCStr()] = cp;
  profiles_mut.unlock();
  ret.push(200);
  ret.push("OK");
  AmArg p;
  p["name"] = args[0]["name"];
  p["md5"] = cp.md5hash;
  p["path"] = args[0]["path"];
  ret.push(p);
}

void SBCFactory::getActiveProfile(const AmArg& args, AmArg& ret) {
  profiles_mut.lock();
  AmArg p;
  for (vector<string>::iterator it=active_profile.begin();
       it != active_profile.end(); it++) {
    p["active_profile"].push(*it);
  }
  profiles_mut.unlock();
  ret.push(200);
  ret.push("OK");
  ret.push(p);
}

void SBCFactory::setActiveProfile(const AmArg& args, AmArg& ret) {
  if (!args[0].hasMember("active_profile")) {
    ret.push(400);
    ret.push("Parameters error: expected ['active_profile': <active_profile list>] ");
    return;
  }
  profiles_mut.lock();
  active_profile = explode(args[0]["active_profile"].asCStr(), ",");
  profiles_mut.unlock();
  ret.push(200);
  ret.push("OK");
  AmArg p;
  p["active_profile"] = args[0]["active_profile"];
  ret.push(p);  
}

void SBCFactory::getRegexMapNames(const AmArg& args, AmArg& ret) {
  AmArg p;
  vector<string> reg_names = regex_mappings.getNames();
  for (vector<string>::iterator it=reg_names.begin();
       it != reg_names.end(); it++) {
    p["regex_maps"].push(*it);
  }
  ret.push(200);
  ret.push("OK");
  ret.push(p);
}

void SBCFactory::setRegexMap(const AmArg& args, AmArg& ret) {
  if (!args[0].hasMember("name") || !args[0].hasMember("file") ||
      !isArgCStr(args[0]["name"]) || !isArgCStr(args[0]["file"])) {
    ret.push(400);
    ret.push("Parameters error: expected ['name': <name>, 'file': <file name>]");
    return;
  }

  string m_name = args[0]["name"].asCStr();
  string m_file = args[0]["file"].asCStr();
  RegexMappingVector v;
  if (!read_regex_mapping(m_file, "=>", "SBC regex mapping", v)) {
    ERROR("reading regex mapping from '%s'\n", m_file.c_str());
    ret.push(401);
    ret.push("Error reading regex mapping from file");
    return;
  }
  regex_mappings.setRegexMap(m_name, v);
  ret.push(200);
  ret.push("OK");
}

void SBCFactory::postControlCmd(const AmArg& args, AmArg& ret) {
  SBCControlEvent* evt;
  if (args.size()<3) {
    evt = new SBCControlEvent(args[1].asCStr());
  } else {
    evt = new SBCControlEvent(args[1].asCStr(), args[2]);
  }
  if (!AmSessionContainer::instance()->postEvent(args[0].asCStr(), evt)) {
    ret.push(404);
    ret.push("Not found");
  } else {
    ret.push(202);
    ret.push("Accepted");
  }
}

SBCDialog::SBCDialog(const SBCCallProfile& call_profile)
  : m_state(BB_Init),
    prepaid_acc(NULL),
    call_profile(call_profile),
    outbound_interface(-1)
{
  set_sip_relay_only(false);
  dlg.reliable_1xx = REL100_IGNORED;
}


SBCDialog::~SBCDialog()
{
  SBCCallRegistry::removeCall(dlg.local_tag);
}

void SBCDialog::onInvite(const AmSipRequest& req)
{
  AmUriParser ruri_parser, from_parser, to_parser;

  DBG("processing initial INVITE\n");

  string app_param = getHeader(req.hdrs, PARAM_HDR, true);

  if(dlg.reply(req, 100, "Connecting") != 0) {
    throw AmSession::Exception(500,"Failed to reply 100");
  }

  ruri = call_profile.ruri.empty() ? 
    req.r_uri : replaceParameters(call_profile.ruri, "RURI", REPLACE_VALS);

  from = call_profile.from.empty() ? 
    req.from : replaceParameters(call_profile.from, "From", REPLACE_VALS);

  to = call_profile.to.empty() ? 
    req.to : replaceParameters(call_profile.to, "To", REPLACE_VALS);

  callid = call_profile.callid.empty() ?
    "" : replaceParameters(call_profile.callid, "Call-ID", REPLACE_VALS);

  if (!call_profile.outbound_proxy.empty()) {
      call_profile.outbound_proxy =
      replaceParameters(call_profile.outbound_proxy, "outbound_proxy", REPLACE_VALS);
      DBG("set outbound proxy to '%s'\n", call_profile.outbound_proxy.c_str());
  }

  if (!call_profile.next_hop_ip.empty()) {
    call_profile.next_hop_ip =
      replaceParameters(call_profile.next_hop_ip, "next_hop_ip", REPLACE_VALS);
    DBG("set next hop ip to '%s'\n", call_profile.next_hop_ip.c_str());

    if (!call_profile.next_hop_port.empty()) {
      call_profile.next_hop_port =
	replaceParameters(call_profile.next_hop_port, "next_hop_port", REPLACE_VALS);
      unsigned int nh_port_i;
      if (str2i(call_profile.next_hop_port, nh_port_i)) {
	ERROR("next hop port '%s' not understood\n", call_profile.next_hop_port.c_str());
	throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
      }
      call_profile.next_hop_port_i = nh_port_i;
      DBG("set next hop port to '%u'\n", call_profile.next_hop_port_i);

      if (!call_profile.next_hop_for_replies.empty()) {
	call_profile.next_hop_for_replies =
	  replaceParameters(call_profile.next_hop_for_replies, "next_hop_for_replies",
			    REPLACE_VALS);
      }
    }
  }

  if (call_profile.rtprelay_enabled) {
    DBG("Enabling RTP relay mode for SBC call\n");

    // force symmetric RTP?
    if (!call_profile.force_symmetric_rtp.empty()) {
      string force_symmetric_rtp =
	replaceParameters(call_profile.force_symmetric_rtp, "force_symmetric_rtp",
			  REPLACE_VALS);
      if (!force_symmetric_rtp.empty() && force_symmetric_rtp != "no"
	  && force_symmetric_rtp != "0") {
	DBG("forcing symmetric RTP (passive mode)\n");
	rtp_relay_force_symmetric_rtp = true;
      }
    }
    // enable symmetric RTP by P-MsgFlags?
    if (!rtp_relay_force_symmetric_rtp) {
      if (call_profile.msgflags_symmetric_rtp) {
	string str_msg_flags = getHeader(req.hdrs,"P-MsgFlags", true);
	unsigned int msg_flags = 0;
	if(reverse_hex2int(str_msg_flags,msg_flags)){
	  ERROR("while parsing 'P-MsgFlags' header\n");
	  msg_flags = 0;
	}
	if (msg_flags & FL_FORCE_ACTIVE) {
	  DBG("P-MsgFlags indicates forced symmetric RTP (passive mode)");
	  rtp_relay_force_symmetric_rtp = true;
	}
      }
    }

    enableRtpRelay(req);
  }

  m_state = BB_Dialing;

  invite_req = req;
  est_invite_cseq = req.cseq;

  removeHeader(invite_req.hdrs,PARAM_HDR);
  removeHeader(invite_req.hdrs,"P-App-Name");

  if (call_profile.sdpfilter_enabled) {
    b2b_mode = B2BMode_SDPFilter;
  }

  if (call_profile.sst_enabled) {
    removeHeader(invite_req.hdrs,SIP_HDR_SESSION_EXPIRES);
    removeHeader(invite_req.hdrs,SIP_HDR_MIN_SE);
  }

  inplaceHeaderFilter(invite_req.hdrs,
		      call_profile.headerfilter_list, call_profile.headerfilter);


  if (!call_profile.append_headers.empty()) {
    string append_headers = replaceParameters(call_profile.append_headers,
					      "append_headers", REPLACE_VALS);
    if (append_headers.size()>2) {
      assertEndCRLF(append_headers);
      invite_req.hdrs+=append_headers;
    }
  }

  if (call_profile.auth_enabled) {
    call_profile.auth_credentials.user = 
      replaceParameters(call_profile.auth_credentials.user, "auth_user", REPLACE_VALS);
    call_profile.auth_credentials.pwd = 
      replaceParameters(call_profile.auth_credentials.pwd, "auth_pwd", REPLACE_VALS);
  }

  if (!call_profile.outbound_interface.empty()) {
    call_profile.outbound_interface = 
      replaceParameters(call_profile.outbound_interface, "outbound_interface",
			REPLACE_VALS);

    if (!call_profile.outbound_interface.empty()) {
      if (call_profile.outbound_interface == "default")
	outbound_interface = 0;
      else {
	map<string,unsigned short>::iterator name_it =
	  AmConfig::If_names.find(call_profile.outbound_interface);
	if (name_it != AmConfig::If_names.end()) {
	  outbound_interface = name_it->second;
	} else {
	  ERROR("selected outbound_interface '%s' does not exist as an interface. "
		"Please check the 'additional_interfaces' "
		"parameter in the main configuration file.",
		call_profile.outbound_interface.c_str());
	  throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
	}
      }
    }
  }

  // get timer
  if (call_profile.call_timer_enabled || call_profile.prepaid_enabled) {
    if (!timersSupported()) {
      ERROR("load session_timer module for call timers\n");
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
  }

  if (call_profile.call_timer_enabled) {
    call_profile.call_timer =
      replaceParameters(call_profile.call_timer, "call_timer", REPLACE_VALS);
    if (str2i(call_profile.call_timer, call_timer)) {
      ERROR("invalid call_timer value '%s'\n", call_profile.call_timer.c_str());
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }

    if (!call_timer) {
      // time=0
      throw AmSession::Exception(503, "Service Unavailable");
    }
  }

  if (call_profile.prepaid_enabled) {
    call_profile.prepaid_accmodule =
      replaceParameters(call_profile.prepaid_accmodule, "prepaid_accmodule", REPLACE_VALS);

    if (!getPrepaidInterface()) {
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }

    call_profile.prepaid_uuid =
      replaceParameters(call_profile.prepaid_uuid, "prepaid_uuid", REPLACE_VALS);

    call_profile.prepaid_acc_dest =
      replaceParameters(call_profile.prepaid_acc_dest, "prepaid_acc_dest", REPLACE_VALS);

    prepaid_starttime = time(NULL);

    AmArg di_args,ret;
    di_args.push(call_profile.prepaid_uuid);
    di_args.push(call_profile.prepaid_acc_dest);
    di_args.push((int)prepaid_starttime);
    di_args.push(getCallID());
    di_args.push(getLocalTag());
    prepaid_acc->invoke("getCredit", di_args, ret);
    prepaid_credit = ret.get(0).asInt();
    if(prepaid_credit < 0) {
      ERROR("Failed to fetch credit from accounting module\n");
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
    if (prepaid_credit == 0) {
      throw AmSession::Exception(402,"Insufficient Credit");
    }
  }

  call_profile.fix_replaces_inv =
      replaceParameters(call_profile.fix_replaces_inv, "fix_replaces_inv", REPLACE_VALS);
  call_profile.fix_replaces_ref =
      replaceParameters(call_profile.fix_replaces_ref, "fix_replaces_ref", REPLACE_VALS);

  if (call_profile.fix_replaces_inv == "yes") {
    fixReplaces(invite_req, true);
  }

#undef REPLACE_VALS

  DBG("SBC: connecting to '%s'\n",ruri.c_str());
  DBG("     From:  '%s'\n",from.c_str());
  DBG("     To:  '%s'\n",to.c_str());
  connectCallee(to, ruri, true);
}

bool SBCDialog::getPrepaidInterface() {
  if (call_profile.prepaid_accmodule.empty()) {
    ERROR("using prepaid but empty prepaid_accmodule!\n");
    return false;
  }
  AmDynInvokeFactory* pp_fact =
    AmPlugIn::instance()->getFactory4Di(call_profile.prepaid_accmodule);
  if (NULL == pp_fact) {
    ERROR("prepaid_accmodule '%s' not loaded\n",
	  call_profile.prepaid_accmodule.c_str());
    return false;
  }
  prepaid_acc = pp_fact->getInstance();
  if(NULL == prepaid_acc) {
    ERROR("could not get a prepaid acc reference\n");
    return false;
  }
  return true;
}

void SBCDialog::process(AmEvent* ev)
{

  AmPluginEvent* plugin_event = dynamic_cast<AmPluginEvent*>(ev);
  if(plugin_event && plugin_event->name == "timer_timeout") {
    int timer_id = plugin_event->data.get(0).asInt();
    if (timer_id == SBC_TIMER_ID_CALL_TIMER &&
	getCalleeStatus() == Connected) {
      DBG("SBC: %us call timer hit - ending call\n", call_timer);
      stopCall();
      ev->processed = true;
      return;
    } else if (timer_id == SBC_TIMER_ID_PREPAID_TIMEOUT) {
      DBG("timer timeout, no more credit\n");
      stopCall();
      ev->processed = true;
      return;
    }
  }

  SBCControlEvent* ctl_event;
  if (ev->event_id == SBCControlEvent_ID &&
      (ctl_event = dynamic_cast<SBCControlEvent*>(ev)) != NULL) {
    onControlCmd(ctl_event->cmd, ctl_event->params);
    return;
  }

  AmB2BCallerSession::process(ev);
}

void SBCDialog::onControlCmd(string& cmd, AmArg& params) {
  if (cmd == "teardown") {
    DBG("teardown requested from control cmd\n");
    stopCall();
    return;
  }
  DBG("ignoring unknown control cmd : '%s'\n", cmd.c_str());
}

int SBCDialog::relayEvent(AmEvent* ev) {
  if (ev->event_id == B2BSipRequest) {

    B2BSipRequestEvent* req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
      assert(req_ev);

    if (call_profile.headerfilter != Transparent) {
      inplaceHeaderFilter(req_ev->req.hdrs,
			  call_profile.headerfilter_list, call_profile.headerfilter);
    }

    if (req_ev->req.method == SIP_METH_REFER &&
	call_profile.fix_replaces_ref == "yes") {
      fixReplaces(req_ev->req, false);
    }

  } else {
    if (ev->event_id == B2BSipReply) {
      if ((call_profile.headerfilter != Transparent) ||
	  (call_profile.reply_translations.size())) {
	B2BSipReplyEvent* reply_ev = dynamic_cast<B2BSipReplyEvent*>(ev);
	assert(reply_ev);
	// header filter
	if (call_profile.headerfilter != Transparent) {
	  inplaceHeaderFilter(reply_ev->reply.hdrs,
			      call_profile.headerfilter_list,
			      call_profile.headerfilter);
	}

	// reply translations
	map<unsigned int, pair<unsigned int, string> >::iterator it =
	  call_profile.reply_translations.find(reply_ev->reply.code);
	if (it != call_profile.reply_translations.end()) {
	  DBG("translating reply %u %s => %u %s\n",
	      reply_ev->reply.code, reply_ev->reply.reason.c_str(),
	      it->second.first, it->second.second.c_str());
	  reply_ev->reply.code = it->second.first;
	  reply_ev->reply.reason = it->second.second;
	}
      }
    }
  }

  return AmB2BCallerSession::relayEvent(ev);
}

int SBCDialog::filterBody(AmSdp& sdp, bool is_a2b) {
  if (call_profile.sdpfilter_enabled) {
    // normalize SDP
    normalizeSDP(sdp);
    // filter SDP
    if (call_profile.sdpfilter != Transparent) {
      filterSDP(sdp, call_profile.sdpfilter, call_profile.sdpfilter_list);
    }
  }
  return 0;
}

void SBCDialog::onSipRequest(const AmSipRequest& req) {
  // AmB2BSession does not call AmSession::onSipRequest for 
  // forwarded requests - so lets call event handlers here
  // todo: this is a hack, replace this by calling proper session 
  // event handler in AmB2BSession
  bool fwd = sip_relay_only &&
    (req.method != "BYE") &&
    (req.method != "CANCEL");
  if (fwd) {
      CALL_EVENT_H(onSipRequest,req);
  }

  if (fwd && call_profile.messagefilter != Transparent) {
    bool is_filtered = (call_profile.messagefilter == Whitelist) ^ 
      (call_profile.messagefilter_list.find(req.method) != 
       call_profile.messagefilter_list.end());
    if (is_filtered) {
      DBG("replying 405 to filtered message '%s'\n", req.method.c_str());
      dlg.reply(req, 405, "Method Not Allowed", "", "", "", SIP_FLAGS_VERBATIM);
      return;
    }
  }

  AmB2BCallerSession::onSipRequest(req);
}

void SBCDialog::onSipReply(const AmSipReply& reply, int old_dlg_status,
			      const string& trans_method)
{
  TransMap::iterator t = relayed_req.find(reply.cseq);
  bool fwd = t != relayed_req.end();

  DBG("onSipReply: %i %s (fwd=%i)\n",reply.code,reply.reason.c_str(),fwd);
  DBG("onSipReply: content-type = %s\n",reply.content_type.c_str());
  if (fwd) {
      CALL_EVENT_H(onSipReply,reply, old_dlg_status, trans_method);
  }

  AmB2BCallerSession::onSipReply(reply,old_dlg_status, trans_method);
}

bool SBCDialog::onOtherReply(const AmSipReply& reply)
{
  bool ret = false;

  if ((m_state == BB_Dialing) && (reply.cseq == invite_req.cseq)) {
    if (reply.code < 200) {
      DBG("Callee is trying... code %d\n", reply.code);
    }
    else if(reply.code < 300) {
      if(getCalleeStatus()  == Connected) {
        m_state = BB_Connected;

	if (!startCallTimer())
	  return ret;

	startPrepaidAccounting();
      }
    }
    else {
      DBG("Callee final error with code %d\n",reply.code);
      ret = AmB2BCallerSession::onOtherReply(reply);
    }
  }
  return ret;
}


void SBCDialog::onOtherBye(const AmSipRequest& req)
{
  stopPrepaidAccounting();
  stopCallTimer();
  AmB2BCallerSession::onOtherBye(req);
}


void SBCDialog::onBye(const AmSipRequest& req)
{
  stopCall();
}


void SBCDialog::onCancel()
{
  if(dlg.getStatus() == AmSipDialog::Pending) {
    stopCall();
    dlg.reply(invite_req, 487, "Request terminated");
  }
}

void SBCDialog::stopCall() {
  if (m_state == BB_Connected) {
    stopPrepaidAccounting();
    stopCallTimer();
  }
  terminateOtherLeg();
  terminateLeg();
}

/** @return whether successful */
bool SBCDialog::startCallTimer() {
  if ((call_profile.call_timer_enabled || call_profile.prepaid_enabled) &&
      (!AmSession::timersSupported())) {
    ERROR("internal implementation error: timers not supported\n");
    terminateOtherLeg();
    terminateLeg();
    return false;
  }

  if (call_profile.call_timer_enabled) {
    DBG("SBC: starting call timer of %u seconds\n", call_timer);
    setTimer(SBC_TIMER_ID_CALL_TIMER, call_timer);
  }

  return true;
}

void SBCDialog::stopCallTimer() {
  if (call_profile.call_timer_enabled) {
    DBG("SBC: removing call timer\n");
    removeTimer(SBC_TIMER_ID_CALL_TIMER);
  }
}

void SBCDialog::startPrepaidAccounting() {
  if (!call_profile.prepaid_enabled)
    return;

  if (NULL == prepaid_acc) {
    ERROR("Internal error, trying to use prepaid, but no prepaid_acc\n");
    terminateOtherLeg();
    terminateLeg();
    return;
  }

  gettimeofday(&prepaid_acc_start, NULL);

  DBG("SBC: starting prepaid timer of %d seconds\n", prepaid_credit);
  {
    setTimer(SBC_TIMER_ID_PREPAID_TIMEOUT, prepaid_credit);
  }

  {
    AmArg di_args,ret;
    di_args.push(call_profile.prepaid_uuid);     // prepaid_uuid
    di_args.push(call_profile.prepaid_acc_dest); // accounting destination
    di_args.push((int)prepaid_starttime);        // call start time (INVITE)
    di_args.push((int)prepaid_acc_start.tv_sec); // call connect time
    di_args.push(getCallID());                   // Call-ID
    di_args.push(getLocalTag());                 // ltag
    di_args.push(other_id);                      // other leg ltag

    prepaid_acc->invoke("connectCall", di_args, ret);
  }
}

void SBCDialog::stopPrepaidAccounting() {
  if (!call_profile.prepaid_enabled)
    return;

  if(prepaid_acc_start.tv_sec != 0 || prepaid_acc_start.tv_usec != 0) {

    if (NULL == prepaid_acc) {
      ERROR("Internal error, trying to subtractCredit, but no prepaid_acc\n");
      return;
    }

    struct timeval now;
    gettimeofday(&now, NULL);
    timersub(&now, &prepaid_acc_start, &now);
    if(now.tv_usec > 500000)
      now.tv_sec++;
    DBG("Call lasted %ld seconds\n", now.tv_sec);

    AmArg di_args,ret;
    di_args.push(call_profile.prepaid_uuid);     // prepaid_uuid
    di_args.push((int)now.tv_sec);               // call duration
    di_args.push(call_profile.prepaid_acc_dest); // accounting destination
    di_args.push((int)prepaid_starttime);        // call start time (INVITE)
    di_args.push((int)prepaid_acc_start.tv_sec); // call connect time
    di_args.push((int)time(NULL));               // call end time
    di_args.push(getCallID());                   // Call-ID
    di_args.push(getLocalTag());                 // ltag
    di_args.push(other_id);

    prepaid_acc->invoke("subtractCredit", di_args, ret);
  }
}

void SBCDialog::createCalleeSession()
{
  SBCCalleeSession* callee_session = new SBCCalleeSession(this, call_profile);
  
  if (call_profile.auth_enabled) {
    // adding auth handler
    AmSessionEventHandlerFactory* uac_auth_f = 
      AmPlugIn::instance()->getFactory4Seh("uac_auth");
    if (NULL == uac_auth_f)  {
      INFO("uac_auth module not loaded. uac auth NOT enabled.\n");
    } else {
      AmSessionEventHandler* h = uac_auth_f->getHandler(callee_session);
      
      // we cannot use the generic AmSessionEventHandler hooks, 
      // because the hooks don't work in AmB2BSession
      callee_session->setAuthHandler(h);
      DBG("uac auth enabled for callee session.\n");
    }
  }

  if (call_profile.sst_enabled) {
    AmSessionEventHandler* h = SBCFactory::session_timer_fact->getHandler(callee_session);
    if(!h) {
      ERROR("could not get a session timer event handler\n");
      delete callee_session;
      throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
    AmConfigReader& sst_cfg = call_profile.use_global_sst_config ? 
      SBCFactory::cfg: call_profile.cfg; // override with profile config

    if(h->configure(sst_cfg)){
      ERROR("Could not configure the session timer: disabling session timers.\n");
      delete h;
    } else {
      callee_session->addHandler(h);
    }
  }

  AmSipDialog& callee_dlg = callee_session->dlg;

  if (!call_profile.outbound_proxy.empty()) {
    callee_dlg.outbound_proxy = call_profile.outbound_proxy;
    callee_dlg.force_outbound_proxy = call_profile.force_outbound_proxy;
  }
  
  if (!call_profile.next_hop_ip.empty()) {
    callee_dlg.next_hop_ip = call_profile.next_hop_ip;
    callee_dlg.next_hop_port = call_profile.next_hop_port.empty() ?
      5060 : call_profile.next_hop_port_i;

    if (!call_profile.next_hop_for_replies.empty()) {
      callee_dlg.next_hop_for_replies =
	(call_profile.next_hop_for_replies == "yes" ||
	 call_profile.next_hop_for_replies == "1");
    }
  }

  if(outbound_interface >= 0)
    callee_dlg.outbound_interface = outbound_interface;

  other_id = AmSession::getNewId();
  
  callee_dlg.local_tag    = other_id;
  callee_dlg.callid       = callid.empty() ?
    AmSession::getNewId() : callid;
  
  // this will be overwritten by ConnectLeg event 
  callee_dlg.remote_party = to;
  callee_dlg.remote_uri   = ruri;

  callee_dlg.local_party  = from; 
  callee_dlg.local_uri    = from; 
  
  DBG("Created B2BUA callee leg, From: %s\n",
      from.c_str());

  if (AmConfig::LogSessions) {
    INFO("Starting B2B callee session %s app %s\n",
	 callee_session->getLocalTag().c_str(), invite_req.cmd.c_str());
  }

  MONITORING_LOG5(other_id.c_str(), 
		  "app",  invite_req.cmd.c_str(),
		  "dir",  "out",
		  "from", callee_dlg.local_party.c_str(),
		  "to",   callee_dlg.remote_party.c_str(),
		  "ruri", callee_dlg.remote_uri.c_str());

  try {
    initializeRTPRelay(callee_session);
  }
  catch (...) {
    delete callee_session;
    throw;
  }

  // A->B
  SBCCallRegistry::addCall(dlg.local_tag, SBCCallRegistryEntry(callee_dlg.callid, callee_dlg.local_tag, ""));
  // B->A
  SBCCallRegistry::addCall(callee_dlg.local_tag, SBCCallRegistryEntry(dlg.callid, dlg.local_tag, dlg.remote_tag));

  callee_session->start();
  
  AmSessionContainer* sess_cont = AmSessionContainer::instance();
  sess_cont->addSession(other_id,callee_session);
}

SBCCalleeSession::SBCCalleeSession(const AmB2BCallerSession* caller,
				   const SBCCallProfile& call_profile) 
  : auth(NULL),
    call_profile(call_profile),
    AmB2BCalleeSession(caller)
{
  dlg.reliable_1xx = REL100_IGNORED;

  if (call_profile.sdpfilter_enabled) {
    b2b_mode = B2BMode_SDPFilter;
  }

}

SBCCalleeSession::~SBCCalleeSession() {
  if (auth) 
    delete auth;

  SBCCallRegistry::removeCall(dlg.local_tag);
}

inline UACAuthCred* SBCCalleeSession::getCredentials() {
  return &call_profile.auth_credentials;
}

int SBCCalleeSession::relayEvent(AmEvent* ev) {
  if (ev->event_id == B2BSipRequest) {

    B2BSipRequestEvent* req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
      assert(req_ev);

    if (call_profile.headerfilter != Transparent) {
      inplaceHeaderFilter(req_ev->req.hdrs,
			  call_profile.headerfilter_list, call_profile.headerfilter);
    }

    if (req_ev->req.method == SIP_METH_REFER &&
	call_profile.fix_replaces_ref == "yes") {
      fixReplaces(req_ev->req, false);
    }

  } else {
    if (ev->event_id == B2BSipReply) {
      if ((call_profile.headerfilter != Transparent) ||
	  (call_profile.reply_translations.size())) {
	B2BSipReplyEvent* reply_ev = dynamic_cast<B2BSipReplyEvent*>(ev);
	assert(reply_ev);

	// header filter
	if (call_profile.headerfilter != Transparent) {
	  inplaceHeaderFilter(reply_ev->reply.hdrs,
			      call_profile.headerfilter_list,
			      call_profile.headerfilter);
	}
	// reply translations
	map<unsigned int, pair<unsigned int, string> >::iterator it =
	  call_profile.reply_translations.find(reply_ev->reply.code);
	if (it != call_profile.reply_translations.end()) {
	  DBG("translating reply %u %s => %u %s\n",
	      reply_ev->reply.code, reply_ev->reply.reason.c_str(),
	      it->second.first, it->second.second.c_str());
	  reply_ev->reply.code = it->second.first;
	  reply_ev->reply.reason = it->second.second;
	}
      }
    }
  }

  return AmB2BCalleeSession::relayEvent(ev);
}

void SBCCalleeSession::process(AmEvent* ev) {
  SBCControlEvent* ctl_event;
  if (ev->event_id == SBCControlEvent_ID &&
      (ctl_event = dynamic_cast<SBCControlEvent*>(ev)) != NULL) {
    onControlCmd(ctl_event->cmd, ctl_event->params);
    return;
  }

  AmB2BCalleeSession::process(ev);
}

void SBCCalleeSession::onSipRequest(const AmSipRequest& req) {
  // AmB2BSession does not call AmSession::onSipRequest for 
  // forwarded requests - so lets call event handlers here
  // todo: this is a hack, replace this by calling proper session 
  // event handler in AmB2BSession
  bool fwd = sip_relay_only &&
    (req.method != "BYE") &&
    (req.method != "CANCEL");
  if (fwd) {
      CALL_EVENT_H(onSipRequest,req);
  }

  if (fwd && call_profile.messagefilter != Transparent) {
    bool is_filtered = (call_profile.messagefilter == Whitelist) ^ 
      (call_profile.messagefilter_list.find(req.method) != 
       call_profile.messagefilter_list.end());
    if (is_filtered) {
      DBG("replying 405 to filtered message '%s'\n", req.method.c_str());
      dlg.reply(req, 405, "Method Not Allowed", "", "", "", SIP_FLAGS_VERBATIM);
      return;
    }
  }

  AmB2BCalleeSession::onSipRequest(req);
}

void SBCCalleeSession::onSipReply(const AmSipReply& reply, int old_dlg_status,
				     const string& trans_method)
{
  // call event handlers where it is not done 
  TransMap::iterator t = relayed_req.find(reply.cseq);
  bool fwd = t != relayed_req.end();
  DBG("onSipReply: %i %s (fwd=%i)\n",reply.code,reply.reason.c_str(),fwd);
  DBG("onSipReply: content-type = %s\n",reply.content_type.c_str());

  if(fwd) {
    CALL_EVENT_H(onSipReply,reply, old_dlg_status, trans_method);
  }

  // update call registry (unfortunately has to be done always -
  // not possible to determine if learned in this reply)
  if (!dlg.remote_tag.empty()) {
    SBCCallRegistry::updateCall(other_id, dlg.remote_tag);
  }

  if (NULL == auth) {
    AmB2BCalleeSession::onSipReply(reply,old_dlg_status, trans_method);
    return;
  }
  
  unsigned int cseq_before = dlg.cseq;
  if (!auth->onSipReply(reply, old_dlg_status, trans_method)) {
    AmB2BCalleeSession::onSipReply(reply, old_dlg_status, trans_method);
  } else {
    if (cseq_before != dlg.cseq) {
      DBG("uac_auth consumed reply with cseq %d and resent with cseq %d; "
          "updating relayed_req map\n", 
	  reply.cseq, cseq_before);
      TransMap::iterator it=relayed_req.find(reply.cseq);
      if (it != relayed_req.end()) {
	relayed_req[cseq_before] = it->second;
	relayed_req.erase(it);
      }
    }
  }
}

void SBCCalleeSession::onSendRequest(const string& method, const string& content_type,
				     const string& body, string& hdrs, int flags, unsigned int cseq)
{
  if (NULL != auth) {
    DBG("auth->onSendRequest cseq = %d\n", cseq);
    auth->onSendRequest(method, content_type,
			body, hdrs, flags, cseq);
  }
  
  AmB2BCalleeSession::onSendRequest(method, content_type,
				     body, hdrs, flags, cseq);
}

void SBCCalleeSession::onControlCmd(string& cmd, AmArg& params) {
  if (cmd == "teardown") {
    DBG("relaying teardown control cmd to A leg\n");
    relayEvent(new SBCControlEvent(cmd, params));
    return;
  }
  DBG("ignoring unknown control cmd : '%s'\n", cmd.c_str());
}

int SBCCalleeSession::filterBody(AmSdp& sdp, bool is_a2b) {
  if (call_profile.sdpfilter_enabled) {
    // normalize SDP
    normalizeSDP(sdp);
    // filter SDP
    if (call_profile.sdpfilter != Transparent) {
      filterSDP(sdp, call_profile.sdpfilter, call_profile.sdpfilter_list);
    }
  }
  return 0;
}

void assertEndCRLF(string& s) {
  if (s[s.size()-2] != '\r' ||
      s[s.size()-1] != '\n') {
    while ((s[s.size()-1] == '\r') ||
	   (s[s.size()-1] == '\n'))
      s.erase(s.size()-1);
    s += "\r\n";
  }
}
