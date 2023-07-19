/* **************************************************************************
 * pfbwt.cpp
 * Output the BWT, the SA (option -S) or the sampled SA (option -s)
 * computed using the prefix-free parsing technique
 *  
 * Usage:
 *   pfbwt[NT][64].x -h
 * for usage info
 *
 * See newscan.cpp for a description of what it does  
 * 
 **************************************************************************** */
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <semaphore.h>
#include <ctime>
#include <string>
#include <fstream>
#include <algorithm>
#include <random>
#include <vector>
#include <map>
extern "C" {
#include "gsa/gsacak.h"
#include "utils.h"
}

using namespace std;
using namespace __gnu_cxx;

// -------------------------------------------------------------
// struct containing command line parameters and other globals
struct Args_Pfbwt {
   const char *name_inputfile;
   uint64_t n;
   string parseExt =  EXTPARSE;    // extension final parse file  
   string occExt =    EXTOCC;      // extension occurrences file  
   string dictExt =   EXTDICT;     // extension dictionary file  
   string lastExt =   EXTLST;      // extension file containing last chars   
   string saExt =     EXTSAI;      // extension file containing sa info   
   int w = 10;            // sliding window size and its default 
   int th = 0;            // number of helper threads, default none 
   bool SA = false;       // output all SA values
   int build_sa_samples = 0;// output sampled SA values
};

// mask for sampled SA: start of a BWT run, end of a BWT run, or both 
#define START_RUN 1
#define END_RUN 2


static long get_num_words(uint8_t *d, long n);
static long binsearch(uint_t x, uint_t a[], long n);
static int_t getlen(uint_t p, uint_t eos[], long n, uint32_t *seqid);
static void compute_dict_bwt_lcp(uint8_t *d, long dsize,long dwords, int w, uint_t **sap, int_t **lcpp, bool log);
static void fwrite_chars_same_suffix(vector<uint32_t> &id2merge,  vector<uint8_t> &char2write, uint32_t *ilist, uint32_t *istart, FILE *fbwt, FILE *fbwtl, long &easy_bwts, long &hard_bwts, bool rle, uint8_t& bwt_last, uint32_t& cur_run_length, uint64_t& num_bwt_runs);
static void fwrite_chars_same_suffix_sa(vector<uint32_t> &id2merge,  vector<uint8_t> &char2write, uint32_t *ilist, uint32_t *istart, FILE *fbwt, long &easy_bwts, long &hard_bwts,
                                     int_t suffixLen, FILE *safile, uint8_t *bwsainfo,long);
static void fwrite_chars_same_suffix_ssa(vector<uint32_t> &id2merge,  vector<uint8_t> &char2write, 
                                    uint32_t *ilist, uint32_t *istart,
                                    FILE *fbwt, FILE *fbwtl, long &easy_bwts, long &hard_bwts,
                                    int_t suffixLen, FILE *iphifile, bool is_64_bit, 
                                    uint8_t *bwsainfo, long n, int &bwtlast, uint64_t &salast, int ssa, bool rle, uint8_t& bwt_last, uint32_t& cur_run_length, uint64_t& num_bwt_runs);
static uint8_t *load_bwsa_info(const Args_Pfbwt &arg, long n);

// class representing the suffix of a dictionary word
// instances of this class are stored to a heap to handle the hard bwts
struct SeqId {
  uint32_t id;       // lex. id of the dictionary word to which the suffix belongs
  int remaining;     // remaining copies of the suffix to be considered  
  uint32_t *bwtpos;  // list of bwt positions of this dictionary word
  uint8_t char2write;// char to be written (is the one preceeding the suffix)

  // constructor
  SeqId(uint32_t i, int r, uint32_t *b, int8_t c) : id(i), remaining(r), bwtpos(b) {
    char2write = c;
  }

  // advance to the next bwt position, return false if there are no more positions 
  bool next() {
    remaining--;
    bwtpos += 1;
    return remaining>0;
  }
  bool operator<(const SeqId& a);
};

bool SeqId::operator<(const SeqId& a) {
    return *bwtpos > *(a.bwtpos);
}

#ifndef NOTHREADS
//#include "pfthreads.hpp"
#endif



/* *******************************************************************
 * Computation of the final BWT
 * 
 * istart[] and islist[] are used together. For each dictionary word i 
 * (considered in lexicographic order) for k=istart[i]...istart[i+1]-1
 * ilist[k] contains the ordered positions in BWT(P) containing word i 
 * ******************************************************************* */
void bwt(const Args_Pfbwt &arg, uint8_t *d, long dsize, // dictionary and its size  
         uint32_t *ilist, uint8_t *last, long psize, // ilist, last and their size 
         uint32_t *istart, long dwords, bool log, uint64_t& num_bwt_runs) // starting point in ilist for each word and # words
{  
  // possibly read bwsa info file and open sa output file
  uint8_t *bwsainfo = load_bwsa_info(arg,psize);
  FILE *safile=NULL, *ssafile=NULL, *esafile=NULL;
  // open the necessary (sampled) SA files (possibly none)
  if(arg.SA) safile = open_aux_file(arg.name_inputfile,EXTSA,"wb");
  //if(arg.build_sa_samples & START_RUN) ssafile = open_aux_file(arg.name_inputfile,EXTSSA,"wb");
  //if(arg.build_sa_samples & END_RUN) esafile = open_aux_file(arg.name_inputfile,EXTESA,"wb");

  FILE *iphifile=NULL;
  if (arg.build_sa_samples) {
    iphifile = open_aux_file(arg.name_inputfile,"iphi","wb");
  }

  // compute sa and bwt of d and do some checking on them 
  uint_t *sa; int_t *lcp;
  compute_dict_bwt_lcp(d,dsize,dwords,arg.w,&sa,&lcp,log);
  // set d[0]==0 as this is the EOF char in the final BWT
  assert(d[0]==Dollar);
  d[0]=0;

  // derive eos from sa. for i=0...dwords-1, eos[i] is the eos position of string i in d
  uint_t *eos = sa+1;
  for(int i=0;i<dwords-1;i++)
    assert(eos[i]<eos[i+1]);

  bool rle = arg.n > dsize;

  // open output file
  FILE *fbwt = NULL;
  FILE *frlen = NULL;
  
  if (rle) {
    fbwt = open_aux_file(arg.name_inputfile,"bwtr","wb");
    frlen = open_aux_file(arg.name_inputfile,"bwtrls","wb");
  } else {
    fbwt = open_aux_file(arg.name_inputfile,"bwt","wb");
  }

  uint32_t cur_run_length = 0;
  uint8_t bwt_last = 0;
  std::pair<uint64_t,uint64_t> I_Phi_first;
  std::pair<uint32_t,uint32_t> I_Phi_cur;
  uint8_t is_64_bit = arg.n > UINT_MAX;
    
  // main loop: consider each entry in the SA of dict
  time_t start = time(NULL);
  long full_words = 0; 
  long easy_bwts = 0;
  long hard_bwts = 0;
  long next;
  uint32_t seqid;
  int lastbwt = Dollar;  // this is certainly not a BWT char
  uint64_t lastSa = UINT64_MAX; // this is an invalid SA entry
  bool first_char = true;
  for(long i=dwords+arg.w+1; i< dsize; i=next ) {
    // we are considering d[sa[i]....]
    next = i+1;  // prepare for next iteration  
    // compute length of this suffix and sequence it belongs
    int_t suffixLen = getlen(sa[i],eos,dwords,&seqid);
    // ignore suffixes of lenght <= w
    if(suffixLen<=arg.w) continue;
    // ----- simple case: the suffix is a full word 
    if(sa[i]==0 || d[sa[i]-1]==EndOfWord) {
      full_words++;
      for(long j=istart[seqid];j<istart[seqid+1];j++) {
        uint8_t nextbwt = last[ilist[j]]; // compute next bwt char
        if (first_char) {
          num_bwt_runs = 1;
          bwt_last = nextbwt;
          first_char = false;
        }
        if(arg.SA) { // full SA requested 
          if(seqid>0) { // if not the first word in the parse output SA values
            uint64_t sa = get_myint(bwsainfo,psize,ilist[j]) - suffixLen;
            if(fwrite(&sa,SABYTES,1,safile)!=1) die("SA write error 0");
          }
          else assert(j==1); // the first word in the parse is the 2nd lex smaller and does not correspond to a SA entry
        }
        else if(arg.build_sa_samples!=0) { // sampled SA
          uint64_t sa=UINT64_MAX; 
          if(seqid>0) { // if not the first word in the parse output (pos,SA[pos]) pair
            if ((arg.build_sa_samples & END_RUN) || (nextbwt!=lastbwt))
              sa = get_myint(bwsainfo,psize,ilist[j]) - suffixLen;
            if(nextbwt!=lastbwt && arg.build_sa_samples) {
              if (is_64_bit) {
                fwrite(&sa,5,1,iphifile);
                fwrite(&lastSa,5,1,iphifile);
              } else {
                I_Phi_cur = std::make_pair(sa,lastSa);
                fwrite(&I_Phi_cur,8,1,iphifile);
              }
            }
          }
          else { // first word in the parsing as a full word. This is the very first BWT char
            sa = get_myint(bwsainfo,psize,0) - arg.w; // this is length of the original text 
            if(arg.build_sa_samples & START_RUN) { // first BWT entry always goes only to the ssa file
              //uint64_t pos = easy_bwts + hard_bwts;
              //assert(pos==0);
              I_Phi_first.first = sa;
              //if(fwrite(&pos,SABYTES,1,ssafile)!=1) die("sampled SA write error 01a");
              //if(fwrite(&sa,SABYTES,1,ssafile)!=1) die("sampled SA write error 01b");
            }
          }
          if(arg.build_sa_samples&END_RUN) lastSa = sa; // save current sa 
        }
        // in any case output BWT char 
        if (rle) {
          if (nextbwt != bwt_last) {
            num_bwt_runs++;
            fputc(bwt_last,fbwt);
            fwrite(&cur_run_length,4,1,frlen);
            bwt_last = nextbwt;
            cur_run_length = 1;
          } else {
            cur_run_length++;
          }
        } else {
          if (nextbwt != bwt_last) {
            bwt_last = nextbwt;
            num_bwt_runs++;
          }
          if(fputc(nextbwt,fbwt)==EOF) die("BWT write error 0");
        }
        lastbwt = nextbwt;   // update lastbwt
        easy_bwts++;
      }
      continue; // proceed with next i 
    }
    // ----- hard case: there can be a group of equal suffixes starting at i
    // save seqid and the corresponding char 
    vector<uint32_t> id2merge(1,seqid); 
    vector<uint8_t> char2write(1,d[sa[i]-1]);
    while(next<dsize && lcp[next]>=suffixLen) {
      assert(lcp[next]==suffixLen);  // the lcp cannot be greater than suffixLen
      assert(sa[next]>0 && d[sa[next]-1]!=EndOfWord); // sa[next] cannot be a full word
      int_t nextsuffixLen = getlen(sa[next],eos,dwords,&seqid);
      assert(nextsuffixLen>=suffixLen);
      if(nextsuffixLen==suffixLen) {
        id2merge.push_back(seqid);           // sequence to consider
        char2write.push_back(d[sa[next]-1]);  // corresponding char
        next++;
      }
      else break;
    }
    // output to fbwt the bwt chars corresponding to the current dictionary suffix, and, if requested, some SA values 
    if(arg.SA)
      fwrite_chars_same_suffix_sa(id2merge,char2write,ilist,istart,fbwt,easy_bwts,hard_bwts,suffixLen,safile,bwsainfo,psize);
    else if(arg.build_sa_samples!=0)
      fwrite_chars_same_suffix_ssa(id2merge,char2write,ilist,istart,fbwt,frlen,easy_bwts,hard_bwts,suffixLen,iphifile,is_64_bit,bwsainfo,psize,lastbwt,lastSa,arg.build_sa_samples,rle,bwt_last,cur_run_length,num_bwt_runs);
    else 
      fwrite_chars_same_suffix(id2merge,char2write,ilist,istart,fbwt,frlen,easy_bwts,hard_bwts,rle,bwt_last,cur_run_length,num_bwt_runs);
  }
  // write very last Sa pair
  if(arg.build_sa_samples & END_RUN) {
    //uint64_t pos = easy_bwts+hard_bwts-1;
    //if(fwrite(&pos,SABYTES,1,esafile)!=1)   die("sampled SA write error 0e");
    //if(fwrite(&lastSa,SABYTES,1,esafile)!=1) die("sampled SA write error 0f");
    I_Phi_first.second = lastSa;

    if (is_64_bit) {
      fwrite(&I_Phi_first.first,5,1,iphifile);
      fwrite(&I_Phi_first.second,5,1,iphifile);
    } else {
      fwrite(&I_Phi_first.first,4,1,iphifile);
      fwrite(&I_Phi_first.second,4,1,iphifile);
    }
  }
  if (rle) {
    fputc(bwt_last,fbwt);
    fwrite(&cur_run_length,4,1,frlen);
  }
  assert(full_words==dwords);
  if (log) cout << "Full words: " << full_words << endl;
  if (log) cout << "Easy bwt chars: " << easy_bwts << endl;
  if (log) cout << "Hard bwt chars: " << hard_bwts << endl;
  if (log) cout << "Generating the final BWT took " << difftime(time(NULL),start) << " wall clock seconds\n";    
  fclose(fbwt);
  if (frlen != NULL) {
    fclose(frlen);
  }
  delete[] lcp;
  delete[] sa;
  if(arg.SA or arg.build_sa_samples!=0) free(bwsainfo);
  if(arg.SA) fclose(safile);
  if(iphifile != NULL) fclose(iphifile);
  //if(arg.build_sa_samples & 2) fclose(esafile);
}  

void print_help(char** argv, const Args_Pfbwt &args) {
  cout << "Usage: " << argv[ 0 ] << " <input filename> [options]" << endl;
  cout << "  Options: " << endl
        << "\t-w W\tsliding window size, def. " << args.w << endl
        #ifndef NOTHREADS
        << "\t-t M\tnumber of helper threads, def. none " << endl
        #endif
        << "\t-h  \tshow help and exit" << endl
        << "\t-s  \tcompute sampled suffix array" << endl
        << "\t-S  \tcompute full suffix array" << endl;
  exit(1);
}

void parseArgs( int argc, char** argv, Args_Pfbwt& arg ) {
  int c;
  extern char *optarg;
  extern int optind;

  puts("==== Command line:");
  for(int i=0;i<argc;i++)
    printf(" %s",argv[i]);
  puts("");

   string sarg;
   while ((c = getopt( argc, argv, "t:w:sehS") ) != -1) {
      switch(c) {
        case 's':
        arg.build_sa_samples |= START_RUN; break; // record SA position for start of runs 
        case 'e':
        arg.build_sa_samples |= END_RUN; break;  // record SA position for end of runs 
        case 'S':
        arg.SA = true; break;
        case 'w':
        sarg.assign( optarg );
        arg.w = stoi( sarg ); break;
        case 't':
        sarg.assign( optarg );
        arg.th = stoi( sarg ); break;
        case 'h':
           print_help(argv, arg); exit(1);
        case '?':
        cout << "Unknown option. Use -h for help." << endl;
        exit(1);
      }
   }
   // the only input parameter is the file name
   arg.name_inputfile = NULL; 
   if (argc == optind+1) 
     arg.name_inputfile = argv[optind];
   else {
      cout << "Invalid number of arguments" << endl;
      print_help(argv,arg);
   }
   // check algorithm parameters
   if(arg.SA && arg.build_sa_samples!=0) {
     cout << "You can either require the sampled SA or the full SA, not both";
     exit(1);
   } 
   if(arg.w <4) {
     cout << "Windows size must be at least 4\n";
     exit(1);
   }
   #ifdef NOTHREADS
   if(arg.th!=0) {
     cout << "The NT version cannot use threads\n";
     exit(1);
   }
   #else
   if(arg.th<0) {
     cout << "Number of threads cannot be negative\n";
     exit(1);
   }
   #endif
}


void bigbwt_pfbwt(const Args_Pfbwt& arg, uint64_t& num_bwt_runs, bool log)
{
  time_t start = time(NULL);  

  // translate command line parameters
  //parseArgs(argc, argv, arg);
  // read dictionary file 
  FILE *g = open_aux_file(arg.name_inputfile,EXTDICT,"rb");
  fseek(g,0,SEEK_END);
  long dsize = ftell(g);
  if(dsize<0) die("ftell dictionary");
  if(dsize<=1+arg.w) die("invalid dictionary file");
  if (log) cout  << "Dictionary file size: " << dsize << endl;
  #if !M64
  if(dsize > 0x7FFFFFFE) {
    if (log) printf("Dictionary size greater than  2^31-2!\n");
    if (log) printf("Please use 64 bit version\n");
    exit(1);
  }
  #endif

  uint8_t *d = new uint8_t[dsize];  
  rewind(g);
  long e = fread(d,1,dsize,g);
  if(e!=dsize) die("fread");
  fclose(g);
  
  // read occ file
  g = open_aux_file(arg.name_inputfile,EXTOCC,"rb");
  fseek(g,0,SEEK_END);
  e = ftell(g);
  if(e<0) die("ftell occ file");
  if(e%4!=0) die("invalid occ file");
  int dwords = e/4;
  if (log) cout  << "Dictionary words: " << dwords << endl;
  uint32_t *occ = new uint32_t[dwords+1];  // dwords+1 since istart overwrites occ
  rewind(g);
  e = fread(occ,4,dwords,g);
  if(e!=dwords) die("fread 2");
  fclose(g);
  assert(dwords==get_num_words(d,dsize));

  // read ilist file 
  g = open_aux_file(arg.name_inputfile,EXTILIST,"rb");
  fseek(g,0,SEEK_END);
  e = ftell(g);
  if(e<0) die("ftell ilist file");
  if(e%4!=0) die("invalid ilist file");
  long psize = e/4;
  if (log) cout  << "Parsing size: " << psize << endl;
  if(psize>0xFFFFFFFEL) die("More than 2^32 -2 words in the parsing");
  uint32_t *ilist = new uint32_t[psize];  
  rewind(g);
  e = fread(ilist,4,psize,g);
  if(e!=psize) die("fread 3");
  fclose(g);
  assert(ilist[0]==1); // EOF is in PBWT[1] 

  // read bwlast file 
  g = open_aux_file(arg.name_inputfile,EXTBWLST,"rb");  
  if (log) cout  << "bwlast file size: " << psize << endl;
  uint8_t *bwlast = new uint8_t[psize];  
  e = fread(bwlast,1,psize,g);
  if(e!=psize) die("fread 4");
  fclose(g);
  
  // convert occ entries into starting positions inside ilist
  // ilist also contains the position of EOF but we don't care about it since it is not in dict 
  uint32_t last=1; // starting position in ilist of the smallest dictionary word  
  for(long i=0;i<dwords;i++) {
    uint32_t tmp = occ[i];
    occ[i] = last;
    last += tmp;
  }
  assert(last==psize);
  occ[dwords]=psize;
  // extra check: the smallest dictionary word is d0 =$.... that occurs once
  assert(occ[1]==occ[0]+1);
  
  // compute and write the final bwt 
  if(arg.th==0)
    bwt(arg,d,dsize,ilist,bwlast,psize,occ,dwords,log,num_bwt_runs); // version not using threads
  else {
    #ifdef NOTHREADS
    cerr << "Sorry, this is the no-threads executable and you requested " << arg.th << " threads\n";
    exit(EXIT_FAILURE);
    #else
    // multithread version
    //bwt_multi(arg,d,dsize,ilist,bwlast,psize,occ,dwords);
    #endif
  }
  delete[] bwlast;
  delete[] ilist;
  delete[] occ;
  delete[] d;  
  if (log) cout << "==== Elapsed time: " << difftime(time(NULL),start) << " wall clock seconds\n";      
}

// --------------------- aux functions ----------------------------------

static uint8_t *load_bwsa_info(const Args_Pfbwt &arg, long n)
{  
  // maybe sa info is not really needed 
  if(arg.SA==false and arg.build_sa_samples==0) return NULL;
  // open .bwsa file for reading and .bwlast for writing
  FILE *fin = open_aux_file(arg.name_inputfile,EXTBWSAI,"rb");
  // allocate and load the bwsa array
  uint8_t *sai = (uint8_t *) malloc(n*IBYTES);
  if(sai==NULL) die("malloc failed (BWSA INFO)"); 
  long s = fread(sai,IBYTES,n,fin);
  if(s!=n) die("bwsa info read");
  if(fclose(fin)!=0) die("bwsa info file close");
  return sai;
}

// compute the number of words in a dictionary
static long get_num_words(uint8_t *d, long n)
{
  long i,num=0;
  for(i=0;i<n;i++)
    if(d[i]==EndOfWord) num++;
  assert(d[n-1]==EndOfDict);
  return num;
}

// binary search for x in an array a[0..n-1] that doesn't contain x
// return the lowest position that is larger than x
static long binsearch(uint_t x, uint_t a[], long n)
{
  long lo=0; long hi = n-1;
  while(hi>lo) {
    assert( ((lo==0) || x>a[lo-1]) && x< a[hi]);
    int mid = (lo+hi)/2;
    assert(x!=a[mid]);  // x is not in a[]
    if(x<a[mid]) hi = mid;
    else lo = mid+1;
  }
  assert(((hi==0) || x>a[hi-1]) && x< a[hi]);
  return hi; 
}


// return the length of the suffix starting in position p.
// also write to seqid the id of the sequence containing that suffix 
// n is the # of distinct words in the dictionary, hence the length of eos[]
static int_t getlen(uint_t p, uint_t eos[], long n, uint32_t *seqid)
{
  assert(p<eos[n-1]);
  *seqid = binsearch(p,eos,n);
  assert(eos[*seqid]> p); // distance between position p and the next $
  return eos[*seqid] - p;
}

// compute the SA and LCP array for the set of (unique) dictionary words
// using gSACA-K. Also do some checking based on the number and order of the special symbols
// d[0..dsize-1] is the dictionary consisting of the concatenation of dictionary words
// in lex order with EndOfWord (0x1) at the end of each word and 
// d[size-1] = EndOfDict (0x0) at the very end. It is d[0]=Dollar (0x2)
// since the first words starts with $. There is another word somewhere
// ending with Dollar^wEndOfWord (it is the last word in the parsing,
// but its lex rank is unknown).  
static void compute_dict_bwt_lcp(uint8_t *d, long dsize,long dwords, int w, 
                          uint_t **sap, int_t **lcpp, bool log) // output parameters
{
  uint_t *sa = new uint_t[dsize];
  int_t *lcp = new int_t[dsize];
  (void) dwords; (void) w;

  if (log) cout  << "Each SA entry: " << sizeof(*sa) << " bytes\n";
  if (log) cout  << "Each LCP entry: " << sizeof(*lcp) << " bytes\n";

  if (log) cout << "Computing SA and LCP of dictionary" << endl; 
  time_t  start = time(NULL);
  gsacak(d,sa,lcp,NULL,dsize);
  if (log) cout << "Computing SA/LCP took " << difftime(time(NULL),start) << " wall clock seconds\n";  
  // ------ do some checking on the sa
  assert(d[dsize-1]==EndOfDict);
  assert(sa[0]==(unsigned long)dsize-1);// sa[0] is the EndOfDict symbol 
  for(long i=0;i<dwords;i++) 
    assert(d[sa[i+1]]==EndOfWord); // there are dwords EndOfWord symbols 
  // EndOfWord symbols are in position order, so the last is d[dsize-2]    
  assert(sa[dwords]==(unsigned long)dsize-2);  
  // there are wsize+1 $ symbols: 
  // one at the beginning of the first word, wsize at the end of the last word
  for(long i=0;i<=w;i++)
    assert(d[sa[i+dwords+1]]==Dollar);         
  // in sa[dwords+w+1] we have the first word in the parsing since that $ is the lex. larger  
  assert(d[0]==Dollar);
  assert(sa[dwords+w+1]==0);
  assert(d[sa[dwords+w+2]]>Dollar);  // end of Dollar chars in the first column
  assert(lcp[dwords+w+2]==0); 
  // copy sa and lcp address
  *sap = sa;  *lcpp = lcp;  
}


// write to the bwt all the characters preceding a given suffix
// doing a merge operation if necessary
static void fwrite_chars_same_suffix(vector<uint32_t> &id2merge,  vector<uint8_t> &char2write, 
                                    uint32_t *ilist, uint32_t *istart,
                                    FILE *fbwt, FILE *fbwtl, long &easy_bwts, long &hard_bwts, bool rle, uint8_t& bwt_last, uint32_t& cur_run_length, uint64_t& num_bwt_runs)
{
  size_t numwords = id2merge.size(); // numwords dictionary words contain the same suffix
  bool samechar=true;
  for(size_t i=1;(i<numwords)&&samechar;i++)
    samechar = (char2write[i-1]==char2write[i]); 
  if(samechar) {
    for(size_t i=0; i<numwords; i++) {
      uint32_t s = id2merge[i];
      if (rle) {
        if (char2write[0] != bwt_last) {
          num_bwt_runs++;
          fputc(bwt_last,fbwt);
          fwrite(&cur_run_length,4,1,fbwtl);
          bwt_last = char2write[0];
          cur_run_length = istart[s+1]-istart[s];
        } else {
          cur_run_length += istart[s+1]-istart[s];
        }
      } else {
        if (char2write[0] != bwt_last) {
          bwt_last = char2write[0];
          num_bwt_runs++;
        }
        for(long j=istart[s];j<istart[s+1];j++) {
          if(fputc(char2write[0],fbwt)==EOF) die("BWT write error 1");
        }
      }
      easy_bwts +=  istart[s+1]- istart[s]; 
    }
  }
  else {  // many words, many chars...     
    vector<SeqId> heap; // create heap
    for(size_t i=0; i<numwords; i++) {
      uint32_t s = id2merge[i];
      heap.push_back(SeqId(s,istart[s+1]-istart[s], ilist+istart[s], char2write[i]));
    }
    std::make_heap(heap.begin(),heap.end());
    while(heap.size()>0) {
      // output char for the top of the heap
      SeqId s = heap.front();
      if (rle) {
        if (s.char2write != bwt_last) {
          num_bwt_runs++;
          fputc(bwt_last,fbwt);
          fwrite(&cur_run_length,4,1,fbwtl);
          bwt_last = s.char2write;
          cur_run_length = 1;
        } else {
          cur_run_length++;
        }
      } else {
        if (s.char2write != bwt_last) {
          bwt_last = s.char2write;
          num_bwt_runs++;
        }
        if(fputc(s.char2write,fbwt)==EOF) die("BWT write error 2");
      }
      
      hard_bwts += 1;
      // remove top 
      pop_heap(heap.begin(),heap.end());
      heap.pop_back();
      // if remaining positions, reinsert to heap
      if(s.next()) {
        heap.push_back(s);
        push_heap(heap.begin(),heap.end());
      }
    }
  }
}

// write to the bwt all the characters preceding a given suffix
// and the corresponding SA entries doing a merge operation
static void fwrite_chars_same_suffix_sa(vector<uint32_t> &id2merge,  vector<uint8_t> &char2write, 
                                    uint32_t *ilist, uint32_t *istart,
                                    FILE *fbwt, long &easy_bwts, long &hard_bwts,
                                    int_t suffixLen, FILE *safile, uint8_t *bwsainfo, long n)
{
  size_t numwords = id2merge.size(); // numwords dictionary words contain the same suffix
  if(numwords==1) {
    uint32_t s = id2merge[0];
    for(long j=istart[s];j<istart[s+1];j++) {
      if(fputc(char2write[0],fbwt)==EOF) die("BWT write error 1");
      uint64_t sa = get_myint(bwsainfo,n,ilist[j]) - suffixLen;
      if(fwrite(&sa,SABYTES,1,safile)!=1) die("SA write error 1");
    }
    easy_bwts +=  istart[s+1]- istart[s];
  }
  else {  // many words, many chars...     
    vector<SeqId> heap; // create heap
    for(size_t i=0; i<numwords; i++) {
      uint32_t s = id2merge[i];
      heap.push_back(SeqId(s,istart[s+1]-istart[s], ilist+istart[s], char2write[i]));
    }
    std::make_heap(heap.begin(),heap.end());
    while(heap.size()>0) {
      // output char for the top of the heap
      SeqId s = heap.front();
      if(fputc(s.char2write,fbwt)==EOF) die("BWT write error 2");
      uint64_t sa = get_myint(bwsainfo,n,*(s.bwtpos)) - suffixLen;
      if(fwrite(&sa,SABYTES,1,safile)!=1) die("SA write error 2");      
      hard_bwts += 1;
      // remove top 
      pop_heap(heap.begin(),heap.end());
      heap.pop_back();
      // if remaining positions, reinsert to heap
      if(s.next()) {
        heap.push_back(s);
        push_heap(heap.begin(),heap.end());
      }
    }
  }
}

// write to the bwt all the characters preceding a given suffix
// and the corresponding sampled SA entries doing a merge operation
static void fwrite_chars_same_suffix_ssa(vector<uint32_t> &id2merge,  vector<uint8_t> &char2write, 
                                    uint32_t *ilist, uint32_t *istart,
                                    FILE *fbwt, FILE *fbwtl, long &easy_bwts, long &hard_bwts,
                                    int_t suffixLen, FILE *iphifile, bool is_64_bit,
                                    uint8_t *bwsainfo, long n, int &bwtlast, uint64_t &salast, int ssa, bool rle, uint8_t& bwt_last, uint32_t& cur_run_length, uint64_t& num_bwt_runs)
{
  size_t numwords = id2merge.size(); // numwords dictionary words contain the same suffix
  if(numwords==1) {
    // there is a single run, so a single potential SA value
    uint32_t s = id2merge[0];
    uint8_t bwtnext = char2write[0];
    if(bwtnext!=bwtlast) {
      if (ssa) {
        uint64_t sa = get_myint(bwsainfo,n,ilist[istart[s]]) - suffixLen;
        if (is_64_bit) {
          fwrite(&sa,5,1,iphifile);
          fwrite(&salast,5,1,iphifile);
        } else {
          std::pair<uint32_t,uint32_t> I_Phi_cur{sa,salast};
          fwrite(&I_Phi_cur,8,1,iphifile);
        }
      }
      bwtlast = bwtnext;
    }
    if (rle) {
      if (bwtnext != bwt_last) {
        num_bwt_runs++;
        fputc(bwt_last,fbwt);
        fwrite(&cur_run_length,4,1,fbwtl);
        bwt_last = bwtnext;
        cur_run_length = istart[s+1]-istart[s];
      } else {
        cur_run_length += istart[s+1]-istart[s];
      }
    } else {
      if (bwtnext != bwt_last) {
        bwt_last = bwtnext;
        num_bwt_runs++;
      }
      for(long j=istart[s];j<istart[s+1];j++) // write all BWT chars
        if(fputc(bwtnext,fbwt)==EOF) die("BWT write error 1");
    }
    easy_bwts +=  istart[s+1]- istart[s];
    if(ssa&END_RUN) // save the last sa value 
      salast = get_myint(bwsainfo,n,ilist[istart[s+1]-1]) - suffixLen;
  }
  else {  // many words, many chars...     
    vector<SeqId> heap; // create heap
    for(size_t i=0; i<numwords; i++) {
      uint32_t s = id2merge[i];
      heap.push_back(SeqId(s,istart[s+1]-istart[s], ilist+istart[s], char2write[i]));
    }
    std::make_heap(heap.begin(),heap.end());
    while(heap.size()>0) {
      // output char for the top of the heap
      uint64_t sa;
      SeqId s = heap.front();
      uint8_t bwtnext = s.char2write; 
      if (rle) {
        if (bwtnext != bwt_last) {
          num_bwt_runs++;
          fputc(bwt_last,fbwt);
          fwrite(&cur_run_length,4,1,fbwtl);
          bwt_last = bwtnext;
          cur_run_length = 1;
        } else {
          cur_run_length++;
        }
      } else {
        if (bwtnext != bwt_last) {
          bwt_last = bwtnext;
          num_bwt_runs++;
        }
        if(fputc(bwtnext,fbwt)==EOF) die("BWT write error 2");
      }
      if ((ssa & END_RUN) || (bwtnext!=bwtlast))
        sa = get_myint(bwsainfo,n,*(s.bwtpos)) - suffixLen;
      if (bwtnext!=bwtlast) {  
        if (is_64_bit) {
          fwrite(&sa,5,1,iphifile);
          fwrite(&salast,5,1,iphifile);
        } else {
          std::pair<uint32_t,uint32_t> I_Phi_cur{sa,salast};
          fwrite(&I_Phi_cur,8,1,iphifile);
        }
        bwtlast = bwtnext; 
      }
      if(ssa & END_RUN) salast = sa; // save current sa 
      hard_bwts += 1;
      // remove top 
      pop_heap(heap.begin(),heap.end());
      heap.pop_back();
      // if remaining positions, reinsert to heap
      if(s.next()) {
        heap.push_back(s);
        push_heap(heap.begin(),heap.end());
      }
    }
  }
}

