/*
 * (C) Copyright 2001 Diomidis Spinellis.
 *
 * A lexical token.
 *
 * Include synopsis:
 * #include <iostream>
 * #include <deque>
 * #include "tokid.h"
 *
 * $Id: token.h,v 1.15 2004/08/07 21:49:01 dds Exp $
 */

#ifndef TOKEN_
#define TOKEN_

// A token part; the smallest unit that is recognised for replacement
class Tpart {
private:
	Tokid ti;		// Token part identifier
	int len;		// Its length
public:
	Tpart() {};
	Tpart(Tokid t, int l): ti(t), len(l) {};
	static void homogenize(const dequeTpart &a, const dequeTpart &b);
	Tokid get_tokid() const { return ti; }
	int get_len() const { return len; }
	friend ostream& operator<<(ostream& o, const Tpart &t);
};

typedef deque<Tpart> dequeTpart;
// Print dequeTpart sequences
ostream& operator<<(ostream& o,const dequeTpart& dt);

class Token {
protected:
	int code;			// Token type code
	dequeTpart parts;		// Identifiers for constituent parts
	string val;			// Token character contents (for identifiers)
public:
	// Unify the constituent equivalence classes for def and ref
	// The definition/reference order is only required when maintaining
	// dependency relationships across files
	static void unify(const Token &def, const Token &ref);

	Token(int icode) : code(icode) {};
	Token(int icode, const string& v)
		: code(icode), val(v)
	{
		parts.push_front(Tpart(Tokid(0, 0), v.length()));
	}
	Token() {};
	// Accessor method
	int get_code() const { return (code); }
	const string& get_val() const { return (val); };
	const string& get_name() const { return (val); };
	// Return the token's symbolic name based on its code
	string name() const;
	// Return the constituent Tokids; they may be more than the parts
	dequeTpart Token::constituents() const;
	// Send it on ostream
	friend ostream& operator<<(ostream& o,const Token &t);
	// Iterators for accessing the token parts
	inline dequeTpart::const_iterator get_parts_begin() const;
	inline dequeTpart::const_iterator get_parts_end() const;
	// Set the token's equivalence class attribute
	void set_ec_attribute(enum e_attribute a) const;
	// Return true if the token contains in its parts the given EC
	bool contains(Eclass *ec) const;
	// Return true if its tokids equal those of stale
	bool Token::equals(const Token &stale) const;
};

dequeTpart::const_iterator
Token::get_parts_begin() const
{
	return parts.begin();
}

dequeTpart::const_iterator
Token::get_parts_end() const
{
	return parts.end();
}

#endif /* TOKEN_ */
