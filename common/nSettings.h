/******************************************************************************
***  n02 v0.3 winnt                                                         ***
**   Open Kaillera Client Core                                               **
***  For latest sources, visit http://sf.net/projects/okai                  ***
******************************************************************************/
#pragma once

class nSettings {
public:
	static void Initialize(char * submo = "p2p", bool global = false);
	static void Terminate();
	static int get_int(char * key, int def_ = -1);
	static char* get_str(char * key, char * buf, char * def_=0);
	static void set_int(char * key, int val);
	static void set_str(char * key, char * val);
	// Same, in an explicit section of n02.ini instead of the mode's own -
	// for what both modes share (the WE Camp account, common/n02_wecamp.cpp).
	static int get_int_in(char * section, char * key, int def_ = -1);
	static char* get_str_in(char * section, char * key, char * buf, char * def_=0);
	static void set_str_in(char * section, char * key, char * val);
	static void set_int_in(char * section, char * key, int val);
};
