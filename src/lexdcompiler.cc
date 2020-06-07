#include "lexdcompiler.h"
#include <unicode/unistr.h>

using namespace icu;
using namespace std;

void expand_alternation(vector<pattern_t> &pats, const vector<pattern_element_t> &alternation);

bool token_t::compatible(const lex_token_t &tok) const
{
  return name.empty() || (subset(tags, tok.tags) && intersectset(negtags, tok.tags).empty());
}
const UnicodeString &LexdCompiler::name(string_ref r) const
{
  return id_to_name[(unsigned int)r];
}

trans_sym_t LexdCompiler::alphabet_lookup(const UnicodeString &symbol)
{
  wstring wsymbol = to_wstring(symbol);
  if(wsymbol.length() == 1)
    return trans_sym_t((int)wsymbol[0]);
  else
  {
    alphabet.includeSymbol(wsymbol);
    return trans_sym_t(alphabet(wsymbol));
  }
}
trans_sym_t LexdCompiler::alphabet_lookup(trans_sym_t l, trans_sym_t r)
{
  return trans_sym_t(alphabet((int)l, (int)r));
}

LexdCompiler::LexdCompiler()
  : shouldAlign(false), shouldCompress(false),
    input(NULL), inLex(false), inPat(false), lineNumber(0), doneReading(false),
    flagsUsed(0), anonymousCount(0)
{
  id_to_name.push_back("");
  name_to_id[""] = string_ref(0);
  lexicons[string_ref(0)] = vector<entry_t>();
}

LexdCompiler::~LexdCompiler()
{}

void
LexdCompiler::die(const wstring &msg)
{
  wcerr << L"Error on line " << lineNumber << ": " << msg << endl;
  exit(EXIT_FAILURE);
}

void LexdCompiler::appendLexicon(string_ref lexicon_id, const vector<entry_t> &to_append)
{
  if(lexicons.find(lexicon_id) == lexicons.end())
    lexicons[lexicon_id] = to_append;
  else
    lexicons[lexicon_id].insert(lexicons[lexicon_id].begin(), to_append.begin(), to_append.end());
}

void
LexdCompiler::finishLexicon()
{
  if(inLex)
  {
    if(currentLexicon.size() == 0) die(L"Lexicon '" + to_wstring(name(currentLexiconId)) + L"' is empty.");
    appendLexicon(currentLexiconId, currentLexicon);
    
    currentLexicon.clear();
  }
  inLex = false;
}

string_ref
LexdCompiler::internName(const UnicodeString& name)
{
  if(name_to_id.find(name) == name_to_id.end())
  {
    name_to_id[name] = string_ref(id_to_name.size());
    id_to_name.push_back(name);
  }
  return name_to_id[name];
}

string_ref
LexdCompiler::checkName(UnicodeString& name)
{
  const static UnicodeString forbidden = " :?|()<>[]*+";
  name.trim();
  int l = name.length();
  if(l == 0) die(L"Unnamed pattern or lexicon");

  for(const auto &c: char_iter(forbidden)) {
    if(name.indexOf(c) != -1) {
      die(L"Lexicon/pattern names cannot contain character '" + to_wstring(c) + L"'");
    }
  }
  return internName(name);
}

lex_seg_t
LexdCompiler::processLexiconSegment(char_iter& iter, UnicodeString& line, unsigned int part_count)
{
  lex_seg_t seg;
  bool inleft = true;
  if((*iter).startsWith(" "))
  {
    if((*iter).length() > 1)
    {
      // if it's a space with a combining diacritic after it,
      // then we want the diacritic
      UnicodeString cur = *iter;
      cur.retainBetween(1, cur.length());
      seg.left.symbols.push_back(alphabet_lookup(cur));
    }
    ++iter;
  }
  if(iter == iter.end() && seg.left.symbols.size() == 0)
    die(L"Expected " + to_wstring(currentLexiconPartCount) + L" parts, found " + to_wstring(part_count));
  for(; iter != iter.end(); ++iter)
  {
    if((*iter).startsWith(" ") || *iter == ']')
      break;
    else if(*iter == "[")
    {
      if(!(inleft ? seg.left : seg.right).tags.empty())
        die(L"Already provided tag list for this side.");
      auto tag_start = (++iter).span();
      for(; *iter != "]"; ++iter)
      {
        if(iter == iter.end())
          die(L"End of line while expecting end tag list ']'");
        if(tag_start == iter.end().span())
        {
          if(*iter == "-")
            die(L"Cannot declare negative tag in lexicon (u16 " + to_wstring(iter.span().first) + L")");
          tag_start = iter.span();
        }
        if(*iter == "," || *iter == " ")
        {
          if(tag_start == iter.span())
            die(L"Empty tag at char " + to_wstring(iter.span().first));
          UnicodeString s = line.tempSubStringBetween(tag_start.first, iter.span().first);
          (inleft ? seg.left : seg.right).tags.insert(checkName(s));
          tag_start = iter.end().span();
        }
      }
      UnicodeString s = line.tempSubStringBetween(tag_start.first, iter.span().first);
      (inleft ? seg.left : seg.right).tags.insert(checkName(s));
    }
    else if(*iter == ":")
    {
      if(inleft)
        inleft = false;
      else
        die(L"Lexicon entry contains multiple colons");
    }
    else if(*iter == "\\")
    {
      (inleft ? seg.left : seg.right).symbols.push_back(alphabet_lookup(*++iter));
    }
    else if(*iter == "{" || *iter == "<")
    {
      UChar end = (*iter == "{") ? '}' : '>';
      int i = iter.span().first;
      for(; iter != iter.end() && *iter != end; ++iter) ;

      if(*iter == end)
      {
        trans_sym_t sym = alphabet_lookup(line.tempSubStringBetween(i, iter.span().second));
        (inleft ? seg.left : seg.right).symbols.push_back(sym);
      }
      else
        die(L"Multichar entry didn't end; searching for " + wstring((wchar_t*)&end, 1));
    }
    else (inleft ? seg.left : seg.right).symbols.push_back(alphabet_lookup(*iter));
  }
  if(inleft)
    seg.right = seg.left;
  return seg;
}

token_t
LexdCompiler::readToken(char_iter& iter, UnicodeString& line)
{
  if(*iter == ' ' || *iter == ':') ++iter;
  auto begin_charspan = iter.span();

  const UnicodeString token_boundary = " )]|<>";
  const UnicodeString token_side_boundary = token_boundary + ":+*?";
  const UnicodeString token_side_name_boundary = token_side_boundary + "([";
  const UnicodeString token_start_decorations = "([";

  for(; iter != iter.end() && token_side_name_boundary.indexOf(*iter) == -1; ++iter);
  UnicodeString name;
  line.extract(begin_charspan.first, (iter == iter.end() ? line.length() : iter.span().first) - begin_charspan.first, name);

  if(name.length() == 0)
    die(L"Symbol '" + to_wstring(*iter) + L"' without lexicon name at u16 " + to_wstring(iter.span().first) + L"-" + to_wstring(iter.span().second-1));

  set<string_ref> tags, negtags;
  unsigned int part = 1;
  if(iter != iter.end() && token_start_decorations.indexOf(*iter) != -1)
  {
    while(iter != iter.end() && token_start_decorations.indexOf(*iter) != -1)
    {
      if(*iter == '(')
      {
        ++iter;
        begin_charspan = iter.span();
        for(; iter != iter.end() && *iter != ')'; ++iter)
        {
          if((*iter).length() != 1 || !u_isdigit((*iter).charAt(0)))
            die(L"Syntax error - non-numeric index in parentheses: " + to_wstring(*iter));
        }
        if(*iter != ')')
          die(L"Syntax error - unmached parenthesis");
        if(iter.span().first == begin_charspan.first)
          die(L"Syntax error - missing index in parenthesis");
        part = (unsigned int)stoul(to_wstring(line.tempSubStringBetween(begin_charspan.first, iter.span().first)));
        ++iter;
      }
      else if(*iter == '[')
      {
        ++iter;
        begin_charspan = iter.span();
        for(; iter != iter.end() && *iter != ']'; ++iter)
        {
          if(iter == iter.end())
            die(L"End of line while expecting end tag list ']'");
          if(begin_charspan == iter.end().span())
            begin_charspan = iter.span();
          if(*iter == "," || *iter == " ")
          {
            if(begin_charspan == iter.span())
              die(L"Empty tag at char " + to_wstring(iter.span().first));
            UnicodeString s = line.tempSubStringBetween(begin_charspan.first, iter.span().first);
            if(s.startsWith("-"))
            {
              s.retainBetween(1, s.length());
              negtags.insert(checkName(s));
            }
            else
              tags.insert(checkName(s));
            begin_charspan = iter.end().span();
          }
        }
	if(*iter != ']')
	  die(L"Syntax error - unmatched ']'");
        UnicodeString s = line.tempSubStringBetween(begin_charspan.first, iter.span().first);
        if(s.startsWith("-"))
        {
          s.retainBetween(1, s.length());
          negtags.insert(checkName(s));
        }
        else
          tags.insert(checkName(s));
        ++iter;
      }
    }
  }
  else if(iter != iter.end() && token_side_boundary.indexOf(*iter) != -1)
  {
    while(iter != iter.begin() && token_side_boundary.indexOf(*iter) != -1)
      --iter;
    iter++;
    while(iter != iter.begin() && token_boundary.indexOf(*iter) != -1)
      --iter;
  }

  return token_t {.name = internName(name), .part = part, .tags = tags, .negtags = negtags};
}

RepeatMode
LexdCompiler::readModifier(char_iter& iter)
{
  if(*iter == "?")
  {
    ++iter;
    return Question;
  }
  else if(*iter == "*")
  {
    ++iter;
    return Star;
  }
  else if(*iter == "+")
  {
    ++iter;
    return Plus;
  }
  return Normal;
}

void
LexdCompiler::processPattern(char_iter& iter, UnicodeString& line)
{
  vector<pattern_t> pats_cur(1);
  vector<pattern_element_t> alternation;
  vector<vector<pattern_t>> patsets_fin;
  vector<vector<pattern_t>> patsets_pref;
  bool final_alternative = true;
  bool sieve_forward = false;
  bool just_sieved = false;
  const UnicodeString boundary = " :()[]+*?|<>";
  const UnicodeString token_boundary = " )|<>";
  const UnicodeString token_side_boundary = token_boundary + ":+*?";
  const UnicodeString token_side_name_boundary = token_side_boundary + "([]";

  for(; iter != iter.end() && *iter != ')' && (*iter).length() > 0; ++iter)
  {
    if(*iter == " ") ;
    else if(*iter == "|")
    {
      if(alternation.empty())
        die(L"Syntax error - initial |");
      final_alternative = false;
    }
    else if(*iter == "<")
    {
      if(sieve_forward)
        die(L"Syntax error - cannot sieve backwards after forwards.");
      if(alternation.empty())
        die(L"Backward sieve without token?");
      expand_alternation(pats_cur, alternation);
      alternation.clear();
      patsets_pref.push_back(pats_cur);
      pats_cur.clear();
      just_sieved = true;
    }
    else if(*iter == ">")
    {
      sieve_forward = true;
      if(alternation.empty())
        die(L"Forward sieve without token?");
      expand_alternation(pats_cur, alternation);
      patsets_fin.push_back(pats_cur);
      alternation.clear();
      just_sieved = true;
    }
    else if(*iter == "[")
    {
      UnicodeString name = UnicodeString::fromUTF8(" " + to_string(anonymousCount++));
      currentLexiconId = internName(name);
      currentLexiconPartCount = 1;
      inLex = true;
      entry_t entry;
      entry.push_back(processLexiconSegment(++iter, line, 0));
      if(*iter == " ") iter++;
      if(*iter != "]")
        die(L"Missing closing ] for anonymous lexicon");
      currentLexicon.push_back(entry);
      finishLexicon();
      if(final_alternative && !alternation.empty())
      {
        expand_alternation(pats_cur, alternation);
        alternation.clear();
      }
      alternation.push_back({
        .left={.name=currentLexiconId, .part=1, .tags=set<string_ref>(), .negtags=set<string_ref>()},
        .right={.name=currentLexiconId, .part=1, .tags=set<string_ref>(), .negtags=set<string_ref>()},
        .mode=readModifier(iter)
      });
      final_alternative = true;
      just_sieved = false;
    }
    else if(*iter == "(")
    {
      string_ref temp = currentPatternId;
      UnicodeString name = UnicodeString::fromUTF8(" " + to_string(anonymousCount++));
      currentPatternId = internName(name);
      ++iter;
      processPattern(iter, line);
      if(*iter == " ")
        *iter++;
      if(iter == iter.end() || *iter != ")")
        die(L"Missing closing ) for anonymous pattern");
      ++iter;
      alternation.push_back({
        .left={.name=currentPatternId, .part=1, .tags=set<string_ref>(), .negtags=set<string_ref>()},
        .right={.name=currentPatternId, .part=1, .tags=set<string_ref>(), .negtags=set<string_ref>()},
        .mode=readModifier(iter)
      });
      currentPatternId = temp;
      final_alternative = true;
      just_sieved = false;
    }
    else
    {
      if(final_alternative && !alternation.empty())
      {
        expand_alternation(pats_cur, alternation);
        alternation.clear();
      }
      token_t left, right;
      if(*iter == ":")
      {
        ++iter;
        right = readToken(iter, line);
      }
      else
      {
        left = readToken(iter, line);
        if(*iter == ":")
        {
          ++iter;
          if(iter != iter.end() && (*iter).length() > 0)
          {
            if(token_boundary.indexOf(*iter) != -1)
              iter--;
            else if (token_side_boundary.indexOf(*iter) == -1)
              right = readToken(iter, line);
          }
        }
        else
          right = left;
      }
      alternation.push_back({.left=left, .right=right,
                             .mode=readModifier(iter)});
      final_alternative = true;
      just_sieved = false;
      if(*iter == ")")
        break;
    }
  }
  if(!final_alternative)
    die(L"Syntax error - trailing |");
  if(just_sieved)
    die(L"Syntax error - trailing sieve (< or >)");
  expand_alternation(pats_cur, alternation);
  patsets_fin.push_back(pats_cur);

  patsets_pref.push_back(vector<pattern_t>(1));

  vector<pattern_t> prefixes;
  vector<pattern_t> last_prefixes(1);
  for(auto it = patsets_pref.rbegin(); it != patsets_pref.rend(); it++)
  {
    vector<pattern_t> new_prefixes;
    new_prefixes.reserve(last_prefixes.size() * it->size());
    prefixes.reserve(prefixes.size() + last_prefixes.size() * it->size());
    for(const auto &last_prefix: last_prefixes)
    {
      for(const auto &new_prefix: *it)
      {
        pattern_t p = new_prefix;
        p.reserve(p.size() + last_prefix.size());
        p.insert(p.end(), last_prefix.begin(), last_prefix.end());
        new_prefixes.push_back(p);
        prefixes.push_back(p);
      }
    }
    last_prefixes = new_prefixes;
  }

  for(const auto &patset_fin: patsets_fin)
  {
    for(const auto &pat: patset_fin)
    {
      for(const auto &pref : prefixes)
      {
        pattern_t p = pref;
        p.reserve(pref.size() + pat.size());
        p.insert(p.end(), pat.begin(), pat.end());
        patterns[currentPatternId].push_back(make_pair(lineNumber, p));
      }
    }
  }
}

void
LexdCompiler::processNextLine()
{
  UnicodeString line;
  UChar c;
  bool escape = false;
  bool comment = false;
  while((c = u_fgetc(input)) != L'\n')
  {
    if(c == U_EOF)
    {
      doneReading = true;
      break;
    }
    if(comment) continue;
    if(escape)
    {
      line += c;
      escape = false;
    }
    else if(c == L'\\')
    {
      escape = true;
      line += c;
    }
    else if(c == L'#')
    {
      comment = true;
    }
    else if(u_isWhitespace(c))
    {
      if(line.length() > 0 && !line.endsWith(' '))
      {
        line += ' ';
      }
    }
    else line += c;
  }
  lineNumber++;
  if(escape) die(L"Trailing backslash");
  if(line.length() == 0) return;

  if(line == "PATTERNS" || line == "PATTERNS ")
  {
    finishLexicon();
    UnicodeString name = " ";
    currentPatternId = internName(name);
    inPat = true;
  }
  else if(line.length() > 7 && line.startsWith("PATTERN "))
  {
    UnicodeString name = line.tempSubString(8);
    finishLexicon();
    currentPatternId = checkName(name);
    inPat = true;
  }
  else if(line.length() > 7 && line.startsWith("LEXICON "))
  {
    UnicodeString name = line.tempSubString(8);
    name.trim();
    finishLexicon();
    currentLexiconPartCount = 1;
    if(name.length() > 1 && name.endsWith(')'))
    {
      UnicodeString num;
      for(int i = name.length()-2; i > 0; i--)
      {
        if(u_isdigit(name[i])) num = name[i] + num;
        else if(name[i] == L'(' && num.length() > 0)
        {
          currentLexiconPartCount = (unsigned int)stoi(to_wstring(num));
          name = name.retainBetween(0, i);
        }
        else break;
      }
      if(name.length() == 0) die(L"Unnamed lexicon");
    }
    currentLexiconId = checkName(name);
    if(lexicons.find(currentLexiconId) != lexicons.end()) {
      if(lexicons[currentLexiconId][0].size() != currentLexiconPartCount) {
        die(L"Multiple incompatible definitions for lexicon '" + to_wstring(name) + L"'");
      }
    }
    inLex = true;
    inPat = false;
  }
  else if(line.length() >= 9 && line.startsWith("ALIAS "))
  {
    finishLexicon();
    if(line.endsWith(' ')) line.retainBetween(0, line.length()-1);
    int loc = line.indexOf(" ", 6);
    if(loc == -1) die(L"Expected 'ALIAS lexicon alt_name'");
    UnicodeString name = line.tempSubString(6, loc-6);
    UnicodeString alt = line.tempSubString(loc+1);
    string_ref altid = checkName(alt);
    string_ref lexid = checkName(name);
    if(lexicons.find(lexid) == lexicons.end()) die(L"Attempt to alias undefined lexicon '" + to_wstring(name) + L"'");
    lexicons[altid] = lexicons[lexid];
    inLex = false;
    inPat = false;
  }
  else if(inPat)
  {
    char_iter iter = char_iter(line);
    processPattern(iter, line);
    if(iter != iter.end() && (*iter).length() > 0)
      die(L"Unexpected " + to_wstring(*iter));
  }
  else if(inLex)
  {
    char_iter iter = char_iter(line);
    entry_t entry;
    for(unsigned int i = 0; i < currentLexiconPartCount; i++)
    {
      entry.push_back(processLexiconSegment(iter, line, i));
    }
    if(*iter == ' ') ++iter;
    if(iter != iter.end())
      die(L"Lexicon entry has '" + to_wstring(*iter) + L"' (found at u16 " + to_wstring(iter.span().first) + L"), more than " + to_wstring(currentLexiconPartCount) + L" components");
    currentLexicon.push_back(entry);
  }
  else die(L"Expected 'PATTERNS' or 'LEXICON'");
}

void
LexdCompiler::buildPattern(int state, Transducer* t, const pattern_t& pat, const vector<int> is_free, unsigned int pos)
{
  if(pos == pat.size())
  {
    t->setFinal(state);
    return;
  }
  const pattern_element_t& tok = pat[pos];
  const bool llex = !tok.left.name || (lexicons.find(tok.left.name) != lexicons.end());
  const bool rlex = !tok.right.name || (lexicons.find(tok.right.name) != lexicons.end());
  if(llex && rlex)
  {
    if(is_free[pos] == 1)
    {
      Transducer *lex = getLexiconTransducer(pat[pos], 0, true);
      if(lex)
      {
        int new_state = t->insertTransducer(state, *lex);
        buildPattern(new_state, t, pat, is_free, pos+1);
      }
      return;
    }
    else if(matchedParts.find(tok.left.name) == matchedParts.end() &&
            matchedParts.find(tok.right.name) == matchedParts.end())
    {
      unsigned int max = lexicons[tok.left.name || tok.right.name].size();
      for(unsigned int index = 0; index < max; index++)
      {
        Transducer *lex = getLexiconTransducer(pat[pos], index, false);
        if(lex)
        {
          int new_state = t->insertTransducer(state, *lex);
          if(new_state == state)
          {
            new_state = t->insertNewSingleTransduction(0, state);
          }
          if(tok.left.name.valid()) matchedParts[tok.left.name] = index;
          if(tok.right.name.valid()) matchedParts[tok.right.name] = index;
          buildPattern(new_state, t, pat, is_free, pos+1);
        }
      }
      if(tok.left.name.valid()) matchedParts.erase(tok.left.name);
      if(tok.right.name.valid()) matchedParts.erase(tok.right.name);
      return;
    }
    if(tok.left.name.valid() && matchedParts.find(tok.left.name) == matchedParts.end())
      matchedParts[tok.left.name] = matchedParts[tok.right.name];
    if(tok.right.name.valid() && matchedParts.find(tok.right.name) == matchedParts.end())
      matchedParts[tok.right.name] = matchedParts[tok.left.name];
    if(tok.left.name.valid() && tok.right.name.valid() && matchedParts[tok.left.name] != matchedParts[tok.right.name])
      die(L"Cannot collate " + to_wstring(name(tok.left.name)) + L" with " + to_wstring(name(tok.right.name)) + L" - both appear in free variation earlier in the pattern.");
    Transducer* lex = getLexiconTransducer(pat[pos], matchedParts[tok.left.name || tok.right.name], false);
    if(lex)
    {
      int new_state = t->insertTransducer(state, *lex);
      buildPattern(new_state, t, pat, is_free, pos+1);
    }
    return;
  }

  bool lpat = (patterns.find(tok.left.name) != patterns.end());
  bool rpat = (patterns.find(tok.right.name) != patterns.end());
  if(lpat && rpat)
  {
    if(tok.left.name != tok.right.name)
      die(L"Cannot collate patterns " + to_wstring(name(tok.left.name)) + L" and " + to_wstring(name(tok.right.name)));
    if(tok.left.part != 1 || tok.right.part != 1)
      die(L"Cannot select part of pattern " + to_wstring(name(tok.left.name)));
    int new_state = t->insertTransducer(state, *buildPattern(tok.left));
    if(tok.mode & Optional)
      t->linkStates(state, new_state, 0);
    if(tok.mode & Repeated)
      t->linkStates(new_state, state, 0);
    buildPattern(new_state, t, pat, is_free, pos+1);
    return;
  }

  // if we get to here it's an error of some kind
  // just have to determine which one
  if((lpat && !tok.right.name) || (rpat && !tok.left.name))
    die(L"Cannot select side of pattern " + to_wstring(name(tok.left.name || tok.right.name)));
  else if((llex && rpat) || (lpat && rlex))
    die(L"Cannot collate lexicon with pattern " + to_wstring(name(tok.left.name)) + L":" + to_wstring(name(tok.right.name)));
  else
  {
    wcerr << "Patterns: ";
    for(auto pat: patterns)
      wcerr << to_wstring(name(pat.first)) << " ";
    wcerr << endl;
    wcerr << "Lexicons: ";
    for(auto l: lexicons)
      wcerr << to_wstring(name(l.first)) << " ";
    wcerr << endl;
    die(L"Lexicon or pattern '" + to_wstring(name(llex ? tok.right.name : tok.left.name)) + L"' is not defined");
  }
}

Transducer*
LexdCompiler::buildPattern(const token_t &tok)
{
  if(tok.part != 1)
    die(L"Cannot build collated pattern " + to_wstring(name(tok.name)));
  if(patternTransducers.find(tok) == patternTransducers.end())
  {
    Transducer* t = new Transducer();
    patternTransducers[tok] = NULL;
    map<string_ref, unsigned int> tempMatch;
    tempMatch.swap(matchedParts);
    for(auto &pat_untagged : patterns[tok.name])
    {
      for(unsigned int i = 0; i < pat_untagged.second.size(); i++)
      {
        auto pat = pat_untagged;
	for(auto &pair: pat.second)
        {
          pair.left.negtags.insert(tok.negtags.begin(), tok.negtags.end());
          pair.right.negtags.insert(tok.negtags.begin(), tok.negtags.end());
        }
	pat.second[i].left.tags.insert(tok.tags.begin(), tok.tags.end());
	pat.second[i].right.tags.insert(tok.tags.begin(), tok.tags.end());

        matchedParts.clear();
        lineNumber = pat.first;
        vector<int> is_free = vector<int>(pat.second.size(), 0);
        for(unsigned int i = 0; i < pat.second.size(); i++)
        {
          if(is_free[i] != 0)
            continue;
          const pattern_element_t& t1 = pat.second[i];
          for(unsigned int j = i+1; j < pat.second.size(); j++)
          {
            const pattern_element_t& t2 = pat.second[j];
            if((t1.left.name.valid() && (t1.left.name == t2.left.name || t1.left.name == t2.right.name)) ||
               (t1.right.name.valid() && (t1.right.name == t2.left.name || t1.right.name == t2.right.name)))
            {
              is_free[i] = -1;
              is_free[j] = -1;
            }
          }
          is_free[i] = (is_free[i] == 0 ? 1 : -1);
        }
        buildPattern(t->getInitial(), t, pat.second, is_free, 0);
      }
    }
    tempMatch.swap(matchedParts);
    t->minimize();
    patternTransducers[tok] = t;
  }
  else if(patternTransducers[tok] == NULL)
  {
    die(L"Cannot compile self-recursive pattern '" + to_wstring(name(tok.name)) + L"'");
  }
  return patternTransducers[tok];
}

/*Transducer*
LexdCompiler::buildPatternWithFlags(UnicodeString name)
{
  UnicodeString letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  if(patternTransducers.find(name) == patternTransducers.end())
  {
    Transducer* t = new Transducer();
    patternTransducers[name] = NULL;
    for(auto& pat : patterns[name])
    {
      map<UnicodeString, UnicodeString> flags;
      map<UnicodeString, int> lexCount;
      vector<UnicodeString> clear;
      for(auto& part : pat.second)
      {
        if(lexCount.find(part.first.first.first) == lexCount.end()) lexCount[part.first.first.first] = 0;
        lexCount[part.first.first.first] += 1;
      }
      lineNumber = pat.first;
      int state = t->getInitial();
      for(auto& part : pat.second)
      {
        UnicodeString& lex = part.first.first.first;
        if(lexicons.find(lex) != lexicons.end())
        {
          if(lexCount[lex] == 1)
          {
            //state = t->insertTransducer(state, *(lexicons[lex]->getTransducerWithFlags(alphabet, part.second.first, part.second.second, L"")));
          }
          else
          {
            if(flags.find(lex) == flags.end())
            {
              unsigned int n = flagsUsed++;
              UnicodeString f;
              while(n > 0 || f.length() == 0)
              {
                f += letters[(int)n%26];
                n /= 26;
              }
              flags[lex] = f;
              clear.push_back(f);
            }
            //state = t->insertTransducer(state, *(lexicons[lex]->getTransducerWithFlags(alphabet, part.second.first, part.second.second, to_wstring(flags[lex]))));
          }
        }
        else if(patterns.find(lex) != patterns.end())
        {
          //if(part.second.first != SideBoth || part.second.second != 1) die(L"Cannot select part or side of pattern '" + to_wstring(lex) + L"'");
          state = t->insertTransducer(state, *(buildPatternWithFlags(lex)));
        }
        else
        {
          die(L"Lexicon or pattern '" + to_wstring(lex) + L"' is not defined");
        }
      }
      for(auto& flag : clear)
      {
        UnicodeString cl = "@C." + flag + "@";
        int s = alphabet_lookup(cl);
        state = t->insertSingleTransduction(alphabet(s, s), state);
      }
      t->setFinal(state);
    }
    t->minimize();
    patternTransducers[name] = t;
  }
  else if(patternTransducers[name] == NULL)
  {
    die(L"Cannot compile self-recursive pattern '" + to_wstring(name) + L"'");
  }
  return patternTransducers[name];
}*/

void
LexdCompiler::readFile(UFILE* infile)
{
  input = infile;
  doneReading = false;
  while(!u_feof(input))
  {
    processNextLine();
    if(doneReading) break;
  }
  finishLexicon();
}

Transducer*
LexdCompiler::buildTransducer(bool usingFlags)
{
  //if(usingFlags) return buildPatternWithFlags(name_to_id[" "]);
  //else return buildPattern(name_to_id[" "]);
  return buildPattern({.name = internName(" "), .part = 1, .tags = set<string_ref>(), .negtags = set<string_ref>()});
}

void expand_alternation(vector<pattern_t> &pats, const vector<pattern_element_t> &alternation)
{
  if(alternation.empty())
    return;
  if(pats.empty())
    pats.push_back(pattern_t());
  vector<pattern_t> new_pats;
  for(const auto &pat: pats)
  {
    for(const auto &tok: alternation)
    {
      auto pat1 = pat;
      pat1.push_back(tok);
      new_pats.push_back(pat1);
    }
  }
  pats = new_pats;
}

void
LexdCompiler::insertEntry(Transducer* trans, const lex_seg_t &seg)
{
  int state = trans->getInitial();
  if(!shouldAlign)
  {
    for(unsigned int i = 0; i < seg.left.symbols.size() || i < seg.right.symbols.size(); i++)
    {
      trans_sym_t l = (i < seg.left.symbols.size()) ? seg.left.symbols[i] : trans_sym_t();
      trans_sym_t r = (i < seg.right.symbols.size()) ? seg.right.symbols[i] : trans_sym_t();
      state = trans->insertSingleTransduction(alphabet((int)l, (int)r), state);
    }
  }
  else
  {
    /*
      This code is adapted from hfst/libhfst/src/parsers/lexc-utils.cc
      It uses the Levenshtein distance algorithm to determine the optimal
      alignment of two strings.
      In hfst-lexc, the priority order for ties is SUB > DEL > INS
      which ensures that 000abc:xyz000 is preferred over abc000:000xyz
      However, we're traversing the strings backwards to simplify extracting
      the final alignment, so we need to switch INS and DEL.
      If shouldCompress is true, we set the cost of SUB to 1 in order to prefer
      a:b over 0:b a:0 without changing the alignment of actual correspondences.
    */
    const unsigned int INS = 0;
    const unsigned int DEL = 1;
    const unsigned int SUB = 2;
    const unsigned int ins_cost = 1;
    const unsigned int del_cost = 1;
    const unsigned int sub_cost = (shouldCompress ? 1 : 100);

    const unsigned int len1 = seg.left.symbols.size();
    const unsigned int len2 = seg.right.symbols.size();
    unsigned int cost[len1+1][len2+1];
    unsigned int path[len1+1][len2+1];
    cost[0][0] = 0;
    path[0][0] = 0;
    for(unsigned int i = 1; i <= len1; i++)
    {
      cost[i][0] = del_cost * i;
      path[i][0] = DEL;
    }
    for(unsigned int i = 1; i <= len2; i++)
    {
      cost[0][i] = ins_cost * i;
      path[0][i] = INS;
    }

    for(unsigned int i = 1; i <= len1; i++)
    {
      for(unsigned int j = 1; j <= len2; j++)
      {
        unsigned int sub = cost[i-1][j-1] + (seg.left.symbols[len1-i] == seg.right.symbols[len2-j] ? 0 : sub_cost);
        unsigned int ins = cost[i][j-1] + ins_cost;
        unsigned int del = cost[i-1][j] + del_cost;

        if(sub <= ins && sub <= del)
        {
          cost[i][j] = sub;
          path[i][j] = SUB;
        }
        else if(ins <= del)
        {
          cost[i][j] = ins;
          path[i][j] = INS;
        }
        else
        {
          cost[i][j] = del;
          path[i][j] = DEL;
        }
      }
    }

    for(unsigned int x = len1, y = len2; (x > 0) || (y > 0);)
    {
      trans_sym_t symbol;
      switch(path[x][y])
      {
        case SUB:
          symbol = alphabet_lookup(seg.left.symbols[len1-x], seg.right.symbols[len2-y]);
          x--;
          y--;
          break;
        case INS:
          symbol = alphabet_lookup(trans_sym_t(), seg.right.symbols[len2-y]);
          y--;
          break;
        default: // DEL
          symbol = alphabet_lookup(seg.left.symbols[len1-x], trans_sym_t());
          x--;
      }
      state = trans->insertSingleTransduction((int)symbol, state);
    }
  }
  trans->setFinal(state);
}

Transducer*
LexdCompiler::getLexiconTransducer(pattern_element_t tok, unsigned int entry_index, bool free)
{
  if(!free && entryTransducers.find(tok) != entryTransducers.end())
    return entryTransducers[tok][entry_index];
  if(free && lexiconTransducers.find(tok) != lexiconTransducers.end())
    return lexiconTransducers[tok];

  vector<entry_t>& lents = lexicons[tok.left.name];
  if(tok.left.name.valid() && tok.left.part > lents[0].size())
    die(to_wstring(name(tok.left.name)) + L"(" + to_wstring(tok.left.part) + L") - part is out of range");
  vector<entry_t>& rents = lexicons[tok.right.name];
  if(tok.right.name.valid() && tok.right.part > rents[0].size())
    die(to_wstring(name(tok.right.name)) + L"(" + to_wstring(tok.right.part) + L") - part is out of range");
  if(tok.left.name.valid() && tok.right.name.valid() && lents.size() != rents.size())
    die(L"Cannot collate " + to_wstring(name(tok.left.name)) + L" with " + to_wstring(name(tok.right.name)) + L" - differing numbers of entries");
  unsigned int count = (tok.left.name.valid() ? lents.size() : rents.size());
  vector<Transducer*> trans;
  if(free)
    trans.push_back(new Transducer());
  else
    trans.reserve(count);
  lex_token_t empty;
  bool did_anything = false;
  for(unsigned int i = 0; i < count; i++)
  {
    if(tok.left.name.valid() && !tok.left.compatible(lents[i][tok.left.part-1].left))
      continue;
    if(tok.right.name.valid() && !tok.right.compatible(rents[i][tok.right.part-1].right))
      continue;
    Transducer* t = free ? trans[0] : new Transducer();
    insertEntry(t, {.left = (tok.left.name.valid() ? lents[i][tok.left.part-1].left : empty),
                   .right = (tok.right.name.valid() ? rents[i][tok.right.part-1].right : empty)});
    did_anything = true;
    if(!free)
    {
      if(tok.mode == Question)
        t->optional();
      else if(tok.mode == Star)
        t->zeroOrMore();
      else if(tok.mode == Plus)
        t->oneOrMore();
      trans.push_back(t);
    }
  }
  if(!did_anything)
    for(auto &t: trans)
      t = NULL;
  if(free)
  {
    if(trans[0])
    {
      trans[0]->minimize();
      if(tok.mode == Question)
        trans[0]->optional();
      else if(tok.mode == Star)
        trans[0]->zeroOrMore();
      else if(tok.mode == Plus)
        trans[0]->oneOrMore();
    }
    lexiconTransducers[tok] = trans[0];
    return trans[0];
  }
  else
  {
    entryTransducers[tok] = trans;
    return trans[entry_index];
  }
}
