#include "autocomplete.h"

using namespace std;

// ---------------- TrieNode ----------------

TrieNode *TrieNode::get(char ch)
{
    return mpp.at(ch); // at(): never inserts a missing key (mpp[ch] would)
}

bool TrieNode::contains(char ch)
{
    if (mpp.find(ch) != mpp.end())
        return true;
    return false;
}

void TrieNode::put(char ch, TrieNode *node)
{
    mpp[ch] = node;
}

void TrieNode::setEnd()
{
    flag = true;
}

bool TrieNode::getEnd()
{
    return flag;
}

int TrieNode::getSize()
{
    return mpp.size();
}

// ---------------- Trie ----------------

Trie::Trie()
{
    root = new TrieNode();
}

Trie::~Trie()
{
    freeNode(root);
}

// deletes a node and everything below it
void Trie::freeNode(TrieNode *node)
{
    for (auto &p : node->mpp)
        freeNode(p.second);
    delete node;
}

void Trie::insert(const string &str)
{
    TrieNode *node = root;
    for (char ch : str)
    {
        if (node->contains(ch))
        {
            node = node->get(ch);
        }
        else
        {
            node->put(ch, new TrieNode());
            node = node->get(ch);
        }
    }
    node->setEnd();
}

string Trie::autocomplete(const string &s)
{
    TrieNode *node = root;
    string str = "";
    for (char ch : s)
    {
        if (node->contains(ch))
        {
            str += ch;
            node = node->get(ch);
        }
        else
            return s;
    }
    if (node->getEnd())
    {
        return str;
    }

    while (node)
    {
        if (node->getEnd())
            return str;
        if (node->getSize() != 1)
            break;
        char key = node->mpp.begin()->first;
        TrieNode *ptr = node->mpp.begin()->second;

        str += key;
        node = ptr;
    }
    return str;
}

// adds every word in the subtree of node; str = the word spelled so far
void Trie::collect(TrieNode *node, string str, vector<string> &words)
{
    if (node->getEnd())
        words.push_back(str);
    for (auto &p : node->mpp) // map: visits children in sorted order
        collect(p.second, str + p.first, words);
}

vector<string> Trie::getWords(const string &s)
{
    vector<string> words;
    TrieNode *node = root;
    for (char ch : s)
    {
        if (node->contains(ch))
            node = node->get(ch);
        else
            return words; // no word starts with s
    }
    collect(node, s, words);
    return words;
}
