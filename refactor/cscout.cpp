/* 
 * (C) Copyright 2001-2003 Diomidis Spinellis.
 *
 * Web-based interface for viewing and processing C code
 *
 * $Id: cscout.cpp,v 1.17 2003/05/24 16:25:27 dds Exp $
 */

#include <map>
#include <string>
#include <deque>
#include <vector>
#include <stack>
#include <iterator>
#include <iostream>
#include <fstream>
#include <list>
#include <set>
#include <cassert>
#include <sstream>		// ostringstream
#include <cstdio>		// perror, rename

#include "swill.h"

#ifdef unix
#include <sys/types.h>		// mkdir
#include <sys/stat.h>		// mkdir
#include <unistd.h>		// unlink
#else
#include <io.h>			// mkdir 
#endif

#include "cpp.h"
#include "ytab.h"
#include "attr.h"
#include "metrics.h"
#include "fileid.h"
#include "tokid.h"
#include "token.h"
#include "ptoken.h"
#include "fchar.h"
#include "error.h"
#include "pltoken.h"
#include "macro.h"
#include "pdtoken.h"
#include "eclass.h"
#include "debug.h"
#include "ctoken.h"
#include "type.h"
#include "stab.h"
#include "license.h"

// Global options
static bool remove_fp;			// Remove common file prefix
static bool sort_rev;			// Reverse sort of identifier names
static bool show_true;			// Only show true identifier properties

// Our identifiers to store as a map
class Identifier {
	string id;		// Identifier name
	string newid;		// New identifier name
	bool xfile;		// True if it crosses files
	bool replaced;		// True if newid has been set
public:
	Identifier(Eclass *e, const string &s) : id(s), replaced(false) {
		xfile = e->sorted_files().size() > 1;
	}
	Identifier() {}
	string get_id() const { return id; }
	void set_newid(const string &s) { newid = s; replaced = true; }
	string get_newid() const { return newid; }
	bool get_xfile() const { return xfile; }
	bool get_replaced() const { return replaced; }
	void set_xfile(bool v) { xfile = v; }
	// To create nicely ordered sets
	inline bool operator ==(const Identifier b) const {
		return (this->id == b.id);
	}
	inline bool operator <(const Identifier b) const {
		return this->id.compare(b.id);
	}
};

typedef map <Eclass *, Identifier> IdProp;

/*
 * Function object to compare IdProp identifier pointers
 * Will compare from end to start if sort_rev is set
 */
struct idcmp
{
	bool operator()(const IdProp::value_type *i1, const IdProp::value_type *i2) const
	{
		if (sort_rev) {
			const string &s1 = (*i1).second.get_id();
			const string &s2 = (*i2).second.get_id();
			string::const_reverse_iterator j1, j2;

			for (j1 = s1.rbegin(), j2 = s2.rbegin();
			     j1 != s1.rend() && j2 != s2.rend(); j1++, j2++)
				if (*j1 != *j2)
					return *j1 < *j2;
			return j1 == s1.rend() && j2 != s2.rend();
		} else
			return (*i1).second.get_id().compare((*i2).second.get_id()) < 0;
	}
};

typedef set <Fileid, fname_order> IFSet;
typedef multiset <const IdProp::value_type *, idcmp> Sids;

static IdProp ids; 
static Attributes::size_type current_project;

void index_page(FILE *of, void *data);

// Return HTML equivalent of character c
static char *
html(char c)
{
	static char str[2];

	switch (c) {
	case '&': return "&amp;";
	case '<': return "&lt;";
	case '>': return "&gt;";
	case ' ': return "&nbsp;";
	case '\t': return "&nbsp;&nbsp;&nbsp;&nbsp;";
	case '\n': return "<br>\n";
	default:
		str[0] = c;
		return str;
	}
}

static string
html(string s)
{
	string r;

	for (string::const_iterator i = s.begin(); i != s.end(); i++)
		r += html(*i);
	return r;
}

// Output s as HTML in of
static void
html_string(FILE *of, string s)
{
	string r;

	for (string::const_iterator i = s.begin(); i != s.end(); i++)
		r += html(*i);
	fprintf(of, r.c_str());
}


// Display an identifier hyperlink
static void
html_id(FILE *of, const IdProp::value_type &i)
{
	fprintf(of, "<a href=\"id.html?id=%u\">%s</a>",
		(unsigned)(i.first),
		(i.second).get_id().c_str());
}

// Add identifiers of the file fi into ids
// Return true if the file contains unused identifiers
static bool
file_analyze(Fileid fi)
{
	ifstream in;
	bool has_unused = false;
	const string &fname = fi.get_path();

	if (DP())
		cout << "Analyze to " << fname << "\n";
	in.open(fname.c_str(), ios::binary);
	if (in.fail()) {
		perror(fname.c_str());
		exit(1);
	}
	// Go through the file character by character
	for (;;) {
		Tokid ti;
		int val;

		ti = Tokid(fi, in.tellg());
		if ((val = in.get()) == EOF)
			break;
		Eclass *ec;
		// Identifiers worth marking
		if ((ec = ti.check_ec())) {
			// Update metrics
			msum.add_id(ec);
			// Worth identifying?
			if (ec->get_size() > 1 || (ec->get_attribute(is_readonly) == false && (
			      ec->get_attribute(is_lscope) || 
			      ec->get_attribute(is_cscope) || 
			      ec->get_attribute(is_macro)))) {
				string s;
				s = (char)val;
				int len = ec->get_len();
				for (int j = 1; j < len; j++)
					s += (char)in.get();
				fi.metrics().process_id(s);
				ids[ec] = Identifier(ec, s);
				if (ec->get_size() == 1)
					has_unused = true;
				continue;
			}
		}
		fi.metrics().process_char((char)val);
	}
	if (DP())
		cout << "nchar = " << fi.metrics().get_nchar() << '\n';
	in.close();
	return has_unused;
}

// Display the contents of a file in hypertext form
// Set show_unused to only mark unused identifiers
static void
file_hypertext(FILE *of, Fileid fi, bool show_unused)
{
	ifstream in;
	const string &fname = fi.get_path();

	if (DP())
		cout << "Write to " << fname << "\n";
	in.open(fname.c_str(), ios::binary);
	if (in.fail()) {
		perror(fname.c_str());
		exit(1);
	}
	// Go through the file character by character
	for (;;) {
		Tokid ti;
		int val;

		ti = Tokid(fi, in.tellg());
		if ((val = in.get()) == EOF)
			break;
		Eclass *ec;
		// Identifiers worth marking
		if ((ec = ti.check_ec())) {
			// Worth marking?
			if (ec->get_size() > 1 || (ec->get_attribute(is_readonly) == false && (
			      ec->get_attribute(is_lscope) || 
			      ec->get_attribute(is_cscope) || 
			      ec->get_attribute(is_macro)))) {
				string s;
				s = (char)val;
				int len = ec->get_len();
				for (int j = 1; j < len; j++)
					s += (char)in.get();
				Identifier i(ec, s);
				const IdProp::value_type ip(ec, i);
				if (show_unused) {
					if (ec->get_size() == 1)
						html_id(of, ip);
					else
						html_string(of, s);
				} else
					html_id(of, ip);
				continue;
			}
		}
		fprintf(of, "%s", html((char)val));
	}
	in.close();
}

// Go through the file doing any replacements needed
// Return the numebr of replacements made
static int
file_replace(Fileid fid)
{
	string plain;
	ifstream in;
	ofstream out;

	in.open(fid.get_path().c_str(), ios::binary);
	if (in.fail()) {
		perror(fid.get_path().c_str());
		exit(1);
	}
	string ofname = fid.get_path() + ".repl";
	out.open(ofname.c_str(), ios::binary);
	if (out.fail()) {
		perror(ofname.c_str());
		exit(1);
	}
	cout << "Processing file " << fid.get_path() << "\n";
	int replacements = 0;
	// Go through the file character by character
	for (;;) {
		Tokid ti;
		int val;

		ti = Tokid(fid, in.tellg());
		if ((val = in.get()) == EOF)
			break;
		Eclass *ec;
		IdProp::const_iterator idi;
		// Identifiers that should be replaced
		if ((ec = ti.check_ec()) &&
		    (idi = ids.find(ec)) != ids.end() &&
		    (*idi).second.get_replaced()) {
			int len = ec->get_len();
			for (int j = 1; j < len; j++)
				(void)in.get();
			out << (*idi).second.get_newid();
			replacements++;
		} else {
			out << (char)val;
		}
	}
	// Needed for Windows
	in.close();
	out.close();
	// Should actually be an assertion
	if (replacements) {
		string cmd("cscout_checkout " + fid.get_path());
		system(cmd.c_str());
		unlink(fid.get_path().c_str());
		rename(ofname.c_str(), fid.get_path().c_str());
		string cmd2("cscout_checkin " + fid.get_path());
		system(cmd2.c_str());
	}
	return replacements;
}

// Create a new HTML file with a given filename and title
static void
html_head(FILE *of, const string fname, const string title)
{
	swill_title(title.c_str());
	if (DP())
		cerr << "Write to " << fname << "\n";
	fprintf(of, 
		"<!doctype html public \"-//IETF//DTD HTML//EN\">\n"
		"<html>\n"
		"<head>\n"
		"<meta name=\"GENERATOR\" content=\"$Id: cscout.cpp,v 1.17 2003/05/24 16:25:27 dds Exp $\">\n"
		"<title>%s</title>\n"
		"</head>\n"
		"<body>\n"
		"<h1>%s</h1>\n", title.c_str(), title.c_str());
}

// And an HTML file end
static void
html_tail(FILE *of)
{
	if (current_project)
		fprintf(of, "<p> <b>Project %s is currently selected</b>\n", Project::get_projname(current_project).c_str());
	fprintf(of, 
		"<p>" 
		"<a href=\"index.html\">Main page</a>\n"
		"</body>"
		"</html>\n");
}

#ifndef COMMERCIAL
/*
 * Return TRUE if the access if from the local host
 * Used to safeguard dangerous operations such as renaming and exiting
 */
static bool
local_access(FILE *fo)
{
	char *peer = swill_getpeerip();

	if (peer && strcmp(peer, "127.0.0.1") == 0)
		return true;
	else {
		html_head(fo, "Remote access", "Remote access not allowed");
		fprintf(fo, "This function can not be executed from a remote host.");
		fprintf(fo, "Make sure you are accessing cscout as localhost or 127.0.0.1.");
		html_tail(fo);
		return false;
	}
}
#endif

// Display a filename on an html file
static void
html_file(FILE *of, Fileid fi)
{
	fprintf(of, "<a href=\"file.html?id=%u\">%s</a>",
		fi.get_id(),
		fi.get_path().c_str());
}

static void
html_file(FILE *of, string fname)
{
	Fileid fi = Fileid(fname);

	html_file(of, fi);
}

// Read-only files
void
rofiles_page(FILE *fo, vector <Fileid> *files)
{
	html_head(fo, "rofiles", "Read-only Files");
	fprintf(fo, "<ul>\n");
	for (vector <Fileid>::iterator i = (*files).begin(); i != (*files).end(); i++) {
		if (current_project && !(*i).get_attribute(current_project)) 
			continue;
		if ((*i).get_readonly() == true) {
			fprintf(fo, "\n<li>");
			html_file(fo, *i);
		}
	}
	fprintf(fo, "\n</ul>\n");
	html_tail(fo);
}

// Writable files
void
wfiles_page(FILE *fo, vector <Fileid> *files)
{
	html_head(fo, "wfiles", "Writable Files");
	fprintf(fo, "<ul>\n");
	for (vector <Fileid>::iterator i = (*files).begin(); i != (*files).end(); i++) {
		if (current_project && !(*i).get_attribute(current_project)) 
			continue;
		if ((*i).get_readonly() == false) {
			fprintf(fo, "\n<li>");
			html_file(fo, *i);
		}
	}
	fprintf(fo, "\n</ul>\n");
	html_tail(fo);
}

// All files
void
afiles_page(FILE *fo, vector <Fileid> *files)
{
	html_head(fo, "afiles", "All Files");
	fprintf(fo, "<ul>\n");
	for (vector <Fileid>::iterator i = (*files).begin(); i != (*files).end(); i++) {
		if (current_project && !(*i).get_attribute(current_project)) 
			continue;
		fprintf(fo, "\n<li>");
		html_file(fo, (*i).get_path());
	}
	fprintf(fo, "\n</ul>\n");
	html_tail(fo);
}


static void
display_sorted_ids(FILE *of, const Sids &sorted_ids)
{
	if (sort_rev)
		fputs("<table><tr><td width=\"50%\" align=\"right\">\n", of);
	else
		fputs("<p>\n", of);

	for (Sids::iterator i = sorted_ids.begin(); i != sorted_ids.end(); i++) {
		html_id(of, **i);
		fputs("<br>\n", of);
	}

	if (sort_rev)
		fputs("</td> <td width=\"50%\"> </td></tr></table>\n", of);
	else
		fputs("</p>\n", of);
}

// All identifiers
void
aids_page(FILE *of, void *p)
{
	Sids sorted_ids;

	html_head(of, "aids", "All Identifiers");
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		if (current_project && !(*i).first->get_attribute(current_project)) 
			continue;
		sorted_ids.insert(&*i);
	}
	display_sorted_ids(of, sorted_ids);
	html_tail(of);
}

// Read-only identifiers
void
roids_page(FILE *of,  void *p)
{
	html_head(of, "roids", "Read-only Identifiers");
	fprintf(of, "<ul>\n");
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		if (current_project && !(*i).first->get_attribute(current_project)) 
			continue;
		if ((*i).first->get_attribute(is_readonly) == true) {
			fprintf(of, "\n<li>");
			html_id(of, *i);
		}
	}
	fprintf(of, "</ul>\n");
	html_tail(of);
}

// Identifier query page
static void
iquery_page(FILE *of,  void *p)
{
	html_head(of, "iquery", "Identifier Query");
	fputs("<FORM ACTION=\"xiquery.html\" METHOD=\"GET\">\n"
	"<input type=\"checkbox\" name=\"writable\" value=\"1\" CHECKED>Writable<br>\n", of);
	for (int i = 0; i < attr_max; i++)
		fprintf(of, "<input type=\"checkbox\" name=\"a%d\" value=\"1\">%s<br>\n", i, 
			Attributes::name(i).c_str());
	fputs(
	"<input type=\"checkbox\" name=\"xfile\" value=\"1\">Crosses file bounary<br>\n"
	"<input type=\"checkbox\" name=\"unused\" value=\"1\">Unused<br>\n"
	"<p>\n"
	"<input type=\"radio\" name=\"match\" value=\"Y\" CHECKED>Match any marked\n"
	"&nbsp; &nbsp; &nbsp; &nbsp;\n"
	"<input type=\"radio\" name=\"match\" value=\"L\">Match all marked\n"
	"&nbsp; &nbsp; &nbsp; &nbsp;\n"
	"<input type=\"radio\" name=\"match\" value=\"E\">Exclude marked\n"
	"&nbsp; &nbsp; &nbsp; &nbsp;\n"
	"<input type=\"radio\" name=\"match\" value=\"T\" >Exact match\n"
	"<br><hr>\n"
	"<table>\n"
	"<tr><td>\n"
	"Identifier names should match RE\n"
	"</td><td>\n"
	"<INPUT TYPE=\"text\" NAME=\"iname\" SIZE=20 MAXLENGTH=256>\n"
	"</td></tr>\n"
	"<tr><td>\n"
	"Select identifiers from filenames matching RE\n"
	"</td><td>\n"
	"<INPUT TYPE=\"text\" NAME=\"fname\" SIZE=20 MAXLENGTH=256>\n"
	"</td></tr>\n"
	"</table>\n"
	"<hr>\n"
	"<p><INPUT TYPE=\"submit\" NAME=\"qi\" VALUE=\"Search identifiers\">\n"
	"</FORM>\n"
	, of);
	html_tail(of);
}

// Process an identifier query
static void
xiquery_page(FILE *of,  void *p)
{
	Sids sorted_ids;
	char match_type;
	vector <bool> match(attr_max);
	bool xfile = !!swill_getvar("xfile");
	bool unused = !!swill_getvar("unused");
	bool writable = !!swill_getvar("writable");

	html_head(of, "xiquery", "Identifier Query Results");

	char *m;
	if (!(m = swill_getvar("match"))) {
		fprintf(of, "Missing value: match");
		return;
	}
	match_type = *m;

	for (int i = 0; i < attr_max; i++) {
		ostringstream varname;

		varname << "a" << i;
		match[i] = !!swill_getvar(varname.str().c_str());
		if (DP())
			cout << "v=[" << varname.str() << "] m=" << match[i] << "\n";
	}
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		if (current_project && !(*i).first->get_attribute(current_project)) 
			continue;
		bool add;
		switch (match_type) {
		case 'Y':	// anY
			add = false;
			for (int j = 0; j < attr_max; j++)
				if (match[j] && (*i).first->get_attribute(j)) {
					add = true;
					break;
				}
			add = (add || (xfile && (*i).second.get_xfile()));
			add = (add || (unused && (*i).first->get_size() == 1));
			add = (add || (writable && !(*i).first->get_attribute(is_readonly)));
			break;
		case 'L':	// alL
			add = true;
			for (int j = 0; j < attr_max; j++)
				if (match[j] && !(*i).first->get_attribute(j)) {
					add = false;
					break;
				}
			add = (add && (!xfile || (*i).second.get_xfile()));
			add = (add && (!unused || (*i).first->get_size() == 1));
			add = (add && (!writable || !(*i).first->get_attribute(is_readonly)));
			break;
		case 'E':	// excludE
			add = true;
			for (int j = 0; j < attr_max; j++)
				if (match[j] && (*i).first->get_attribute(j)) {
					add = false;
					break;
				}
			add = (add && (!xfile || !(*i).second.get_xfile()));
			add = (add && (!unused || !((*i).first->get_size() == 1)));
			add = (add && (!writable || (*i).first->get_attribute(is_readonly)));
			break;
		case 'T':	// exactT
			add = true;
			for (int j = 0; j < attr_max; j++)
				if (match[j] != (*i).first->get_attribute(j)) {
					add = false;
					break;
				}
			add = (add && (xfile == (*i).second.get_xfile()));
			add = (add && (unused == ((*i).first->get_size() == 1)));
			add = (add && (writable == !(*i).first->get_attribute(is_readonly)));
			break;
		}
		if (add)
			sorted_ids.insert(&*i);
	}
	display_sorted_ids(of, sorted_ids);
	fputs("<p>You can bookmark this page to save the respective query<p>", of);
	html_tail(of);
}

// Display an identifier property
static void
show_id_prop(FILE *fo, const string &name, bool val)
{
	if (!show_true || val)
		fprintf(fo, ("<li>" + name + ": %s\n").c_str(), val ? "Yes" : "No");
}

// Details for each identifier
void
identifier_page(FILE *fo, void *p)
{
	Eclass *e;
	if (!swill_getargs("p(id)", &e)) {
		fprintf(fo, "Missing value");
		return;
	}
	char *subst;
	Identifier &id = ids[e];
	if ((subst = swill_getvar("sname"))) {
#ifndef COMMERCIAL
		if (!local_access(fo))
			return;
#endif
		// Passing subst directly core-dumps under
		// gcc version 2.95.4 20020320 [FreeBSD 4.7]
		string ssubst(subst);
		id.set_newid(ssubst);
	}
	html_head(fo, "id", string("Identifier: ") + html(id.get_id()));
	fprintf(fo, "<FORM ACTION=\"id.html\" METHOD=\"GET\">\n<ul>\n");
	for (int i = 0; i < attr_max; i++)
		show_id_prop(fo, Attributes::name(i), e->get_attribute(i));
	show_id_prop(fo, "Crosses file boundary", id.get_xfile());
	show_id_prop(fo, "Unused", e->get_size() == 1);
	fprintf(fo, "<li> Appears in project(s): \n<ul>\n");
	if (DP()) {
		cout << "First project " << attr_max << "\n";
		cout << "Last project " <<  Attributes::get_num_attributes() - 1 << "\n";
	}
	for (Attributes::size_type j = attr_max; j < Attributes::get_num_attributes(); j++)
		if (e->get_attribute(j))
			fprintf(fo, "<li>%s\n", Project::get_projname(j).c_str());
	fprintf(fo, "</ul>\n");
	if (id.get_replaced())
		fprintf(fo, "<li> Substituted with: [%s]\n", id.get_newid().c_str());
	if (!e->get_attribute(is_readonly)) {
		fprintf(fo, "<li> Substitute with: \n"
			"<INPUT TYPE=\"text\" NAME=\"sname\" SIZE=10 MAXLENGTH=256> "
			"<INPUT TYPE=\"submit\" NAME=\"repl\" VALUE=\"Substitute\">\n");
		fprintf(fo, "<INPUT TYPE=\"hidden\" NAME=\"id\" VALUE=\"%u\">\n", (unsigned)e);
	}
	fprintf(fo, "</ul>\n");
	IFSet ifiles = e->sorted_files();
	fprintf(fo, "<h2>Dependent Files (Writable)</h2>\n");
	fprintf(fo, "<ul>\n");
	for (IFSet::const_iterator j = ifiles.begin(); j != ifiles.end(); j++) {
		if ((*j).get_readonly() == false) {
			fprintf(fo, "\n<li>");
			html_file(fo, (*j).get_path());
		}
	}
	fprintf(fo, "</ul>\n");
	fprintf(fo, "<h2>Dependent Files (All)</h2>\n<ul>\n");
	for (IFSet::const_iterator j = ifiles.begin(); j != ifiles.end(); j++) {
		fprintf(fo, "\n<li>");
		html_file(fo, (*j).get_path());
	}
	fprintf(fo, "</ul>\n");
	fprintf(fo, "</FORM>\n");
	html_tail(fo);
}

// Writable identifiers
void
wids_page(FILE *fo, void *p)
{
	html_head(fo, "wids", "Writable Identifiers");
	fprintf(fo, "<ul>\n");
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		if (current_project && !(*i).first->get_attribute(current_project)) 
			continue;
		if ((*i).first->get_attribute(is_readonly) == false) {
			fprintf(fo, "\n<li>");
			html_id(fo, *i);
		}
	}
	fprintf(fo, "</ul>\n");
	html_tail(fo);
}

// Cross-file writable identifiers
void
xids_page(FILE *fo, void *p)
{
	html_head(fo, "xids", "File-spanning Writable Identifiers");
	fprintf(fo, "<ul>\n");
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		if (current_project && !(*i).first->get_attribute(current_project)) 
			continue;
		if ((*i).second.get_xfile() == true &&
		    (*i).first->get_attribute(is_readonly) == false) {
			fprintf(fo, "\n<li>");
			html_id(fo, *i);
		}
	}
	fprintf(fo, "</ul>\n");
	html_tail(fo);
}

// Unused project-scoped writable identifiers
void
upids_page(FILE *fo, void *p)
{
	html_head(fo, "upids", "Unused Project-scoped Writable Identifiers");
	fprintf(fo, "<ul>\n");
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		Eclass *e = (*i).first;
		if (current_project && !e->get_attribute(current_project)) 
			continue;
		if (
		    e->get_size() == 1 &&
		    e->get_attribute(is_lscope) == true &&
		    e->get_attribute(is_readonly) == false) {
			fprintf(fo, "\n<li>");
			html_id(fo, *i);
		}
	}
	fprintf(fo, "</ul>\n");
	html_tail(fo);
}

// Unused file-scoped writable identifiers
void
ufids_page(FILE *fo, void *p)
{
	html_head(fo, "ufids", "Unused File-scoped Writable Identifiers");
	fprintf(fo, "<ul>\n");
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		Eclass *e = (*i).first;
		if (current_project && !e->get_attribute(current_project)) 
			continue;
		if (
		    e->get_size() == 1 &&
		    e->get_attribute(is_cscope) == true &&
		    e->get_attribute(is_readonly) == false) {
			fprintf(fo, "\n<li>");
			html_id(fo, *i);
		}
	}
	fprintf(fo, "</ul>\n");
	html_tail(fo);
}

// Unused macro writable identifiers
void
umids_page(FILE *fo, void *p)
{
	html_head(fo, "umids", "Unused Macro Writable Identifiers");
	fprintf(fo, "<ul>\n");
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		Eclass *e = (*i).first;
		if (current_project && !e->get_attribute(current_project)) 
			continue;
		if (
		    e->get_size() == 1 &&
		    e->get_attribute(is_macro) == true &&
		    e->get_attribute(is_readonly) == false) {
			fprintf(fo, "\n<li>");
			html_id(fo, *i);
		}
	}
	fprintf(fo, "</ul>\n");
	html_tail(fo);
}


// Front-end global options page
void
options_page(FILE *fo, void *p)
{
	html_head(fo, "options", "Global Options");
	fprintf(fo, "<FORM ACTION=\"soptions.html\" METHOD=\"GET\">\n");
	fprintf(fo, "<input type=\"checkbox\" name=\"remove_fp\" value=\"1\" %s>Remove common path prefix from files<br>\n", (remove_fp ? "checked" : ""));
	fprintf(fo, "<input type=\"checkbox\" name=\"sort_rev\" value=\"1\" %s>Sort identifiers starting from their last character<br>\n", (sort_rev ? "checked" : ""));
	fprintf(fo, "<input type=\"checkbox\" name=\"show_true\" value=\"1\" %s>Show only true identifier classes (brief view)<br>\n", (show_true ? "checked" : ""));
/*
Do not show No in identifier properties (option)

*/
	fprintf(fo, "<p><INPUT TYPE=\"submit\" NAME=\"set\" VALUE=\"OK\">\n");
	fprintf(fo, "<INPUT TYPE=\"submit\" NAME=\"set\" VALUE=\"Cancel\">\n");
	fprintf(fo, "<INPUT TYPE=\"submit\" NAME=\"set\" VALUE=\"Apply\">\n");
	fprintf(fo, "</FORM>\n");
	html_tail(fo);
}

// Front-end global options page
void
set_options_page(FILE *fo, void *p)
{
	if (string(swill_getvar("set")) == "Cancel") {
		index_page(fo, p);
		return;
	}
	remove_fp = !!swill_getvar("remove_fp");
	sort_rev = !!swill_getvar("sort_rev");
	show_true = !!swill_getvar("show_true");
	if (string(swill_getvar("set")) == "Apply")
		options_page(fo, p);
	else
		index_page(fo, p);
}

// Display all projects, allowing user to select
void
select_project_page(FILE *fo, void *p)
{
	html_head(fo, "sproject", "Select Active Project");
	fprintf(fo, "<ul>\n");
	fprintf(fo, "<li> <a href=\"setproj.html?projid=0\">All projects</a>\n");
	for (Attributes::size_type j = attr_max; j < Attributes::get_num_attributes(); j++)
		fprintf(fo, "<li> <a href=\"setproj.html?projid=%u\">%s</a>\n", (unsigned)j, Project::get_projname(j).c_str());
	fprintf(fo, "\n</ul>\n");
	html_tail(fo);
}

// Select a single project (or none) to restrict file/identifier results
void
set_project_page(FILE *fo, void *p)
{
#ifndef COMMERCIAL
	if (!local_access(fo))
		return;
#endif

	if (!swill_getargs("i(projid)", &current_project)) {
		fprintf(fo, "Missing value");
		return;
	}
	index_page(fo, p);
}


// Index
void
index_page(FILE *of, void *data)
{
	html_head(of, "index", "CScout Results");
	fprintf(of, 
		"<ul>\n"
		"<li> <a href=\"afiles.html\">All files</a>\n"
		"<li> <a href=\"rofiles.html\">Read-only files</a>\n"
		"<li> <a href=\"wfiles.html\">Writable files</a>\n"
		"<li> <a href=\"aids.html\">All identifiers</a>\n"
		"<li> <a href=\"roids.html\">Read-only identifiers</a>\n"
		"<li> <a href=\"wids.html\">Writable identifiers</a>\n"
		"<li> <a href=\"xids.html\">File-spanning writable identifiers</a>\n"
		"<li> <a href=\"upids.html\">Unused project-scoped writable identifiers</a>\n"
		"<li> <a href=\"ufids.html\">Unused file-scoped writable identifiers</a>\n"
		"<li> <a href=\"umids.html\">Unused macro writable identifiers</a>\n"
		"<li> <a href=\"iquery.html\">Identifier query</a>\n"
		"</ul>"
		"<h2>Operations</h2>"
		"<ul>\n"
		"<li> <a href=\"options.html\">Global options</a>\n"
		"<li> <a href=\"sproject.html\">Select active project</a>\n"
		"<li> <a href=\"sexit.html\">Exit - saving changes</a>\n"
		"<li> <a href=\"qexit.html\">Exit - ignore changes</a>\n"
		"</ul>");
	html_tail(of);
}

void
file_page(FILE *of, void *p)
{
	ostringstream fname;
	int id;
	if (!swill_getargs("i(id)", &id)) {
		fprintf(of, "Missing value");
		return;
	}
	Fileid i(id);
	const string &pathname = i.get_path();
	fname << i.get_id();
	html_head(of, "file", string("File: ") + html(pathname));
	fprintf(of, "<ul>\n");
	fprintf(of, "<li> Read-only: %s", i.get_readonly() ? "Yes" : "No");
	fprintf(of, "\n<li> Number of characters: %d", i.metrics().get_nchar());
	fprintf(of, "\n<li> Comment characters: %d", i.metrics().get_nccomment());
	fprintf(of, "\n<li> Space characters: %d", i.metrics().get_nspace());
	fprintf(of, "\n<li> Number of line comments: %d", i.metrics().get_nlcomment());
	fprintf(of, "\n<li> Number of block comments: %d", i.metrics().get_nbcomment());
	fprintf(of, "\n<li> Number of lines: %d", i.metrics().get_nline());
	fprintf(of, "\n<li> Length of longest line: %d", i.metrics().get_maxlinelen());
	fprintf(of, "\n<li> Number of preprocessor directives: %d", i.metrics().get_nppdirective());
	fprintf(of, "\n<li> Number of directly included files: %d", i.metrics().get_nincfile());
	fprintf(of, "\n<li> Number of defined functions: %d", i.metrics().get_nfunction());
	fprintf(of, "\n<li> Number of C statements: %d", i.metrics().get_nstatement());
	fprintf(of, "\n<li> Number of C strings: %d", i.metrics().get_nstring());
	fprintf(of, "\n<li> Used in project(s): \n<ul>");
	for (Attributes::size_type j = attr_max; j < Attributes::get_num_attributes(); j++)
		if (i.get_attribute(j))
			fprintf(of, "<li>%s\n", Project::get_projname(j).c_str());
	fprintf(of, "</ul>\n<li> <a href=\"src.html?id=%s\">Source code</a>\n", fname.str().c_str());
	fprintf(of, "\n<li> <a href=\"usrc.html?id=%s\">Source code with unused non-local writable identifiers marked</a>\n", fname.str().c_str());
	fprintf(of, "</ul>\n");
	html_tail(of);
}

void
source_page(FILE *of, void *p)
{
	ostringstream fname;
	int id;
	if (!swill_getargs("i(id)", &id)) {
		fprintf(of, "Missing value");
		return;
	}
	Fileid i(id);
	const string &pathname = i.get_path();
	fname << i.get_id();
	html_head(of, "src", string("Source: ") + html(pathname));
	file_hypertext(of, i, false);
	html_tail(of);
}

void
unused_source_page(FILE *of, void *p)
{
	ostringstream fname;
	int id;
	if (!swill_getargs("i(id)", &id)) {
		fprintf(of, "Missing value");
		return;
	}
	Fileid i(id);
	const string &pathname = i.get_path();
	fname << i.get_id();
	html_head(of, "usrc", string("Source with unused non-local writable identifiers marked: ") + html(pathname));
	file_hypertext(of, i, true);
	html_tail(of);
}

static bool must_exit = false;

void
write_quit_page(FILE *of, void *p)
{
#ifndef COMMERCIAL
	if (!local_access(of))
		return;
#endif
	// Determine files we need to process
	IFSet process;
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		if ((*i).second.get_replaced()) {
			Eclass *e = (*i).first;
			IFSet ifiles = e->sorted_files();
			process.insert(ifiles.begin(), ifiles.end());
		}
	}
	// Now do the replacements
	int replacements = 0;
	for (IFSet::const_iterator i = process.begin(); i != process.end(); i++)
		replacements += file_replace(*i);
	html_head(of, "quit", "CScout exiting");
	fprintf(of, "A total of %d replacements were made in %d files.", replacements, process.size());
	fprintf(of, "<p>Bye...</body></html>");
	must_exit = true;
}

void
quit_page(FILE *of, void *p)
{
#ifndef COMMERCIAL
	if (!local_access(of))
		return;
#endif
	html_head(of, "quit", "CScout exiting");
	fprintf(of, "No changes were saved.");
	fprintf(of, "<p>Bye...</body></html>");
	must_exit = true;
}

#ifdef COMMERCIAL
/*
 * Parse the access control list .cscout_acl.
 * The ACL is searched in three different directories:
 * ., $CSCOUT_HOME, and $HOME
 */
static void
parse_acl()
{
	
	ifstream in;
	string ad, host;
	vector <char *> dirs;
	dirs.push_back(".");
	dirs.push_back(getenv("CSCOUT_HOME"));
	dirs.push_back(getenv("HOME"));
	vector <char *>::const_iterator i;
	string fname;

	for (i = dirs.begin(); i != dirs.end(); i++) {
		if (!*i)
			continue;
		fname = string(*i) + "/.cscout_acl";
		in.open(fname.c_str());
		if (in.fail())
			in.clear();
		else {
			cout << "Parsing ACL from " << fname << "\n";
			for (;;) {
				in >> ad;
				if (in.eof())
					break;
				in >> host;
				if (ad == "A") {
					cout << "Allow from IP address " << host << "\n";
					swill_allow(host.c_str());
				} else if (ad == "D") {
					cout << "Deny from IP address " << host << "\n";
					swill_deny(host.c_str());
				} else
					cout << "Bad ACL specification " << ad << " " << host << "\n";
			}
			break;
		}
	}
	if (i == dirs.end()) {
		cout << "No ACL found.  Only localhost access will be allowed.\n";
		swill_allow("127.0.0.1");
	}
}
#endif

int
main(int argc, char *argv[])
{
	Pdtoken t;

	Debug::db_read();
	if (!swill_init(8081)) {
		cerr << "Couldn't initialize the SWILL server.\n";
		exit(1);
	}

	license_init();

#ifdef COMMERCIAL
	parse_acl();
#endif

	// Pass 1: process master file loop
	Fchar::set_input(argv[1]);
	do
		t.getnext();
	while (t.get_code() != EOF);

	// Pass 2: Create web pages
	vector <Fileid> files = Fileid::sorted_files();

	swill_handle("sproject.html", select_project_page, 0);
	swill_handle("options.html", options_page, 0);
	swill_handle("soptions.html", set_options_page, 0);
	swill_handle("sexit.html", write_quit_page, 0);
	swill_handle("qexit.html", quit_page, 0);
	swill_handle("afiles.html", afiles_page, &files);
	swill_handle("rofiles.html", rofiles_page, &files);
	swill_handle("wfiles.html", wfiles_page, &files);

	// Populate the EC identifier member
	for (vector <Fileid>::iterator i = files.begin(); i != files.end(); i++) {
		ostringstream fname;
		fname << (*i).get_id();
		/* bool has_unused = */ file_analyze(*i);
	}


	// Set xfile and  metrics for each identifier
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		Eclass *e = (*i).first;
		IFSet ifiles = e->sorted_files();
		(*i).second.set_xfile(ifiles.size() > 1);

		ostringstream fname;
		fname << (unsigned)e;

		// Update metrics
		msum.add_unique_id(e);
	}


	// Update fle metrics
	msum.summarize_files();

#ifdef COMMERCIAL
	license_check("");
#else
	/* 
	 * Send the metrics
	 * up to 10 project names
	 * up 50 cross-file identifiers 
	 */
	ostringstream mstring;
	mstring << msum;
	mstring << "\nxids: ";
	int count = 0;
	for (IdProp::iterator i = ids.begin(); i != ids.end(); i++) {
		if ((*i).second.get_xfile() == true)
			mstring << (*i).second.get_id() << ' ';
		if (count++ > 100)
			break;
	}
	mstring << "\nprojnames: ";
	count = 0;
	for (Attributes::size_type j = attr_max; j < Attributes::get_num_attributes(); j++) {
		mstring << Project::get_projname(j) << ' ';
		if (count++ > 10)
			break;
	}
	mstring << "\n";
	license_check(mstring.str().c_str());
#endif

#ifndef PRODUCTION
	if (CORRECTION_FACTOR - license_offset != 0) {
		cout << "**********Unable to obtain correct license*********\n";
		cout << "license_offset = " << license_offset << "\n";
	}
#endif

	swill_handle("src.html", source_page, NULL);
	swill_handle("usrc.html", unused_source_page, NULL);
	swill_handle("file.html", file_page, NULL);

	swill_handle("iquery.html", iquery_page, NULL);
	swill_handle("xiquery.html", xiquery_page, NULL);
	swill_handle("aids.html", aids_page, NULL);
	swill_handle("roids.html", roids_page, NULL);
	swill_handle("wids.html", wids_page, NULL);
	swill_handle("xids.html", xids_page, NULL);
	swill_handle("upids.html", upids_page, NULL);
	swill_handle("ufids.html", ufids_page, NULL);
	swill_handle("umids.html", umids_page, NULL);

	swill_handle("id.html", identifier_page, NULL);
	swill_handle("setproj.html", set_project_page, NULL);
	swill_handle("index.html", (void (*)(FILE *, void *))((char *)index_page - CORRECTION_FACTOR + license_offset), 0);

	// Serve web pages
	cout << "We are now ready to serve you at http://localhost:8081\n";
	while (!must_exit)
		swill_serve();

	return (0);
}