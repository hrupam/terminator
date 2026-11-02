#pragma once
#include <map>
#include <string>
#include <vector>

class TrieNode
{
public:
    std::map<char, TrieNode *> mpp; // map, not unordered_map: children stay in sorted order
    bool flag = false;

public:
    TrieNode *get(char ch);
    bool contains(char ch);
    void put(char ch, TrieNode *node);
    void setEnd();
    bool getEnd();
    int getSize();
};

class Trie
{
private:
    TrieNode *root;

    void freeNode(TrieNode *node);
    void collect(TrieNode *node, std::string str, std::vector<std::string> &words);

public:
    Trie();
    ~Trie();
    Trie(const Trie &) = delete; // owns raw pointers: a copy would delete them twice
    Trie &operator=(const Trie &) = delete;

    void insert(const std::string &str);
    std::string autocomplete(const std::string &s);           // extend to the longest common prefix
    std::vector<std::string> getWords(const std::string &s); // every word starting with s, sorted
};
