#ifndef __AMSIPMSG_H__
#define __AMSIPMSG_H__

#include <string>
using std::string;

#include "sip/trans_layer.h"
#include "AmArg.h"

/* enforce common naming in Req&Rpl */
class _AmSipMsgInDlg
  : public ArgObject
{
 public:
  string       method;
  string       route;

  string       contact;
  string       content_type;

  string       via1;
  string       hdrs;
  string       body;
  unsigned int cseq;
  unsigned int rseq;
  string       callid;

  // transaction ticket from sip stack
  trans_ticket tt;

  string         remote_ip;
  unsigned short remote_port;
  string         local_ip;
  unsigned short local_port;

  _AmSipMsgInDlg() : cseq(0), rseq(0) { }
  virtual ~_AmSipMsgInDlg() { };

  virtual string print() const = 0;
};

/** \brief represents a SIP reply */
class AmSipReply : public _AmSipMsgInDlg
{
 public:
  unsigned int code;
  string       reason;
  string       next_request_uri;

  /*TODO: this should be merged with request's from_/to_tag and moved above*/
  string       remote_tag;
  string       local_tag;


 AmSipReply() : code(0), _AmSipMsgInDlg() { }
  ~AmSipReply() { }
  string print() const;
};


/** \brief represents a SIP request */
class AmSipRequest : public _AmSipMsgInDlg
{
 public:
  string cmd;

  string user;
  string domain;
  string r_uri;
  string from_uri;
  string from;
  string to;
  string from_tag;
  string to_tag;

  string rack_method;
  unsigned int rack_cseq;
  string via_branch;

 AmSipRequest() : _AmSipMsgInDlg() { }
  ~AmSipRequest() { }
  
  string print() const;
};

string getHeader(const string& hdrs,const string& hdr_name, bool single = false);

string getHeader(const string& hdrs,const string& hdr_name, 
		 const string& compact_hdr_name, bool single = false);

/** find a header, starting from char skip
    if found, value is between pos1 and pos2 
    and hdr start is the start of the header 
    @return true if found */
bool findHeader(const string& hdrs,const string& hdr_name, const size_t skip, 
		size_t& pos1, size_t& pos2, 
		size_t& hdr_start);

bool removeHeader(string& hdrs, const string& hdr_name);

/** add an option tag @param tag to list @param hdr_name */
void addOptionTag(string& hdrs, const string& hdr_name, const string& tag);

/** remove an option tag @param tag from list @param hdr_name */
void removeOptionTag(string& hdrs, const string& hdr_name, const string& tag);

#endif /* __AMSIPMSG_H__ */


/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 2
 * End:
 */
