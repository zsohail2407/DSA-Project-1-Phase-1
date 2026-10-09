// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <sstream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
      top=nullptr;
      count=0;
    }
    void push(const T &val)
    {
        if(count<MAX_STACK_DEPTH)
        {
      Node*temp=new Node;
      temp->data=val;
      temp->next=top;
      top=temp;
      count++;
        }
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
      Node* temp=top;
      T v=temp->data;
      top=top->next;
      delete temp;
      count--;
      return v;
      
        // pop the top value on the stack
    }
    T &peek()
    {
        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return count==0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* temp=top;
        int ct=0;
        while (temp!=nullptr&& ct<maxLen)
        {
            out[ct]=temp->data;
            temp=temp->next;
            ct++;
        }
        return ct;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};


class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head=nullptr;
        tail=nullptr;
        stepCount=0;
    }
    void record(Snapshot *s)
    {
        TimelineNode* temp=new TimelineNode;
        temp->data=s;
        temp->next=nullptr;
        temp->prev=tail;
        if(head==nullptr)
        {
           head=temp;
        }
        else
        {
           // TimelineNode* t2=tail;
            tail->next=temp;
        }
            tail=temp;
            stepCount++;
        // add record in the timeline
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    string aaaa;
    while(getline(in,aaaa))
    {
       stringstream aa(aaaa);
       string word;
       if(aa>>word)
       {
        out=aaaa;
        return true;
       }
    }
    return false;
}
string firstWord(const string &line)
{
    // returns first word from the input string
       stringstream aa(line);
       string word;
       if(aa>>word)
       {
        return word;
       }
       return "";
}
string secondWord(const string &line)
{
       stringstream aa(line);
       string word1,word2;
       if(aa>>word1>>word2)
       {
        return word2;
       }
       return "";
    // returns the second word
}
bool validateProgram(const char *sourcePath)
{
    ifstream file(sourcePath);
    string aaaa;
    bool check=false;
      if(!file.is_open())
      {
        return false;
      }
    while(readSourceLine(file,aaaa))
    {
        //apnay liay: check tru repr. func and false repr. func end
      string aa=firstWord(aaaa);
      if(aa=="func")
      {
    if (check==true)
    {
        file.close();
        return false;
    }
    check=true;
      }
      else if(aa=="func_end")
      {
    if (check==false)
    {
        file.close();
        return false;
    }
    check=false;
      }
    }
    file.close();
    return !check;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t start=ftell(f);
    int32_t size=text.length();
    fwrite(&offsetField,8,1,f);
    fwrite(&size,4,1,f);
    fwrite(&text[0],1,size,f);
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    return start;
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offsetField=0;
    int32_t size=0;
    if(fread(&offsetField,8,1,f)!=1)
    {
        return -1;
    }
    if(fread(&size,4,1,f)!=1)
    {
        return -1;
    }
    outText.resize(size);
    fread(&outText[0],1,size,f);
    return offsetField;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    int64_t mainO=-1;
    ifstream fin(sourcePath);
    FILE* fout=fopen(resolveBinPath,"wb");
    if(!fin.is_open()|| fout==NULL)
    {
        return -1;
    }
    string aaaa;
    while(readSourceLine(fin,aaaa))
    {
        string word=firstWord(aaaa);
        int64_t position=ftell(fout);
        if(word=="func")
        {
            funcArray[funcCount].funcName=secondWord(aaaa);
            funcArray[funcCount].byteOffsetInResolveBin=position;
            funcCount++;
            writeResolveRecord(fout,position,aaaa);
        }
        else if(word=="call")
        {
            patches[patchCount].targetFuncName=secondWord(aaaa);
            patches[patchCount].byteOffsetOfOffsetField=position;
            patchCount++;
            writeResolveRecord(fout,0,aaaa);
        }
        else
        {
            writeResolveRecord(fout,position,aaaa);
        }
    }
    for(int32_t i=0;i<patchCount;i++)
    {
        int64_t target=-1;
        for(int32_t j=0;j<funcCount;j++)
        {
        if(funcArray[j].funcName== patches[i].targetFuncName)
        {
            target=funcArray[j].byteOffsetInResolveBin;
        }
        }
        if(target==-1)
        {
            fclose(fout);
            return -1;
        }
        fseek(fout,patches[i].byteOffsetOfOffsetField,SEEK_SET);
        fwrite(&target,8,1,fout);
    }
        for(int32_t a=0;a<funcCount;a++)
        {
            if(funcArray[a].funcName== "main")
            {
                mainO=funcArray[a].byteOffsetInResolveBin;
            }
        }
        fclose(fout);
        if(mainO==-1)
        {
            return -1;
        }
return mainO;
    

    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    stringstream aa(line);
    string w;
    int32_t n=0;
    while(n<maxTokens && aa>>w)
    {
        tokens[n].text=w;
        if(n==0)
        {
           tokens[n].type=KEYWORD;
        }
        else if(n==1)
        {
           tokens[n].type=IDENTIFIER;
        }
        else
        {
            tokens[n].type=PARAM;
        }
        n++;
    }
    return n;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot* aaaaa= new Snapshot;
    aaaaa->stackDepth=callStack.snapshot_into(aaaaa->callStack, MAX_STACK_DEPTH);
    return aaaaa;
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}


