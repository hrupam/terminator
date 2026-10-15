#include <string>
#include "string_helper.h"

using namespace std;

string processCommand(const string &str)
{
    int n = str.length();
    string final = "";
    int i = 0;
    while (i < n)
    {
        while (i < n && str[i] == ' ')
            i++;
        if (i == n)
            break;
        if (!final.empty())
            final += " ";
        while (i < n && str[i] != ' ')
        {
            final += str[i];
            i++;
        }
    }
    return final;
}

bool startsWith(const string &str, const string &prefix)
{
    return str.rfind(prefix, 0) == 0;
}