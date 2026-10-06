#include "charm_timezone.h"
#include <stdio.h>
#include <string.h>
bool charm_timezone_resolve(const char *name,char *out,size_t size) {
    static const struct {const char *name,*rule;} zones[]={
        {"UTC","UTC0"},{"Etc/UTC","UTC0"},
        {"Asia/Shanghai","CST-8"},{"Asia/Hong_Kong","HKT-8"},
        {"Asia/Tokyo","JST-9"},{"Asia/Singapore","SGT-8"},
        {"America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0"},
        {"America/New_York","EST5EDT,M3.2.0,M11.1.0"},
        {"Europe/London","GMT0BST,M3.5.0/1,M10.5.0/2"},
        {"Europe/Berlin","CET-1CEST,M3.5.0/2,M10.5.0/3"},
        {"Australia/Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3"},
    };
    if(!name || !out || !size) return false;
    for(unsigned i=0;i<sizeof(zones)/sizeof(zones[0]);i++) if(!strcmp(name,zones[i].name))
        return snprintf(out,size,"%s",zones[i].rule)<(int)size;
    // UTC+08:00 means local time is UTC plus eight hours; POSIX reverses signs.
    if(strlen(name)!=9 || strncmp(name,"UTC",3) || (name[3]!='+' && name[3]!='-') || name[6]!=':') return false;
    if(name[4]<'0'||name[4]>'9'||name[5]<'0'||name[5]>'9'||name[7]<'0'||name[7]>'9'||name[8]<'0'||name[8]>'9') return false;
    unsigned hour=(name[4]-'0')*10+name[5]-'0',minute=(name[7]-'0')*10+name[8]-'0';
    if(hour>14 || minute>59 || (hour==14 && minute)) return false;
    return snprintf(out,size,"UTC%c%u:%02u",name[3]=='+'?'-':'+',hour,minute)<(int)size;
}
