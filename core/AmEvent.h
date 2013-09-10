/*
 * Copyright (C) 2002-2003 Fhg Fokus
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
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
/** @file AmEvent.h */
#ifndef AmEvent_h
#define AmEvent_h

#include "AmArg.h"

#include <string>
using std::string;

#define E_PLUGIN 100
#define E_SYSTEM 101

/** \brief base event class */
struct AmEvent
{
  int event_id;
  bool processed;

  AmEvent(int event_id);
  AmEvent(const AmEvent& rhs);

  virtual ~AmEvent();

  virtual AmEvent* clone();
};

/** 
 * \brief named event for inter-plugin-API 
 *
 * Optionally the AmPluginEvent also holds a dynamic argument array.
 */
struct AmPluginEvent: public AmEvent
{
  string      name;
  AmArg       data;

  AmPluginEvent(const string& n)
    : AmEvent(E_PLUGIN), name(n), data() {}

  AmPluginEvent(const string& n, const AmArg& d)
    : AmEvent(E_PLUGIN), name(n), data(d) {}
};

/**
 * \brief named event for system events (e.g. server stopped) 
 */
struct AmSystemEvent : public AmEvent 
{
  enum EvType {
    ServerShutdown = 0,
    User1,
    User2
  };

  EvType sys_event;

  AmSystemEvent(EvType e)
    : AmEvent(E_SYSTEM), sys_event(e) { }

  AmSystemEvent(const AmSystemEvent& rhs) 
    : AmEvent(rhs), sys_event(rhs.sys_event) { }

  AmEvent* clone() {  return new AmSystemEvent(*this); };

  static const char* getDescription(EvType t) {
    switch (t) {
    case ServerShutdown: return "ServerShutdown";
    case User1: return "User1";
    case User2: return "User2";
    default: return "Unknown";
    }
  }
};

/** \brief event handler interface */
class AmEventHandler
{
 public:
  virtual void process(AmEvent*)=0;
  virtual ~AmEventHandler() { };
};

#endif
