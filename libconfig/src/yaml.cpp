// YAML -> Layer with yaml-cpp (system library). The only file that sees yaml-cpp types.
//
// We use yaml-cpp's event API (YAML::Parser + EventHandler), not YAML::Node: Node
// silently merges duplicate keys and resolves aliases, and we need to reject both. The
// events are collected into a flat node table (no recursion, so no deep destructor
// chains), then walked with a bounded depth.
#include "lm/config/config.hpp"

#include <yaml-cpp/depthguard.h>
#include <yaml-cpp/emitterstyle.h>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/mark.h>
#include <yaml-cpp/parser.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace lm::config {

namespace {

constexpr int kMaxDepth = 16;                    // settings nesting (a.b.c...)
constexpr std::size_t kMaxBytes = 64 * 1024;     // a config file is a few hundred bytes
constexpr int kMaxFlowNesting = 32;              // [ { ... before parsing (stack bound)

struct Node {
    enum Kind { Null, Scalar, Seq, Map, Alias } kind = Null;
    int line = 0, col = 0; // 1-based; 0 = unknown
    std::size_t pos = 0;   // offset in the text (for the quoted-with-tag check)
    bool hasPos = false;
    std::string tag;       // "?" plain untagged, "!" quoted/block untagged, else explicit
    bool anchored = false;
    bool flow = false;     // Seq/Map written in flow style ([...], {...})
    std::string value;
    std::vector<std::size_t> kids; // Seq: items; Map: key, value, key, value, ...
};

void setMark(Node &n, const YAML::Mark &m) {
    if (m.is_null()) return;
    n.line = m.line >= 0 ? m.line + 1 : 0;
    n.col = m.column >= 0 ? m.column + 1 : 0;
    if (m.pos >= 0) n.pos = std::size_t(m.pos), n.hasPos = true;
}

// Builds one document's nodes. Also counts documents (a second one is an error).
struct Collector : YAML::EventHandler {
    std::vector<Node> nodes;
    std::vector<std::size_t> open; // stack of Seq/Map being filled
    std::size_t root = 0;
    bool haveRoot = false;
    int docs = 0;

    std::size_t add(Node n) {
        nodes.push_back(std::move(n));
        const std::size_t id = nodes.size() - 1;
        if (!open.empty()) nodes[open.back()].kids.push_back(id);
        else if (!haveRoot) root = id, haveRoot = true;
        return id;
    }
    void OnDocumentStart(const YAML::Mark &) override { ++docs; }
    void OnDocumentEnd() override {}
    void OnNull(const YAML::Mark &m, YAML::anchor_t a) override {
        Node n;
        setMark(n, m);
        n.anchored = a != YAML::NullAnchor;
        add(std::move(n));
    }
    void OnAlias(const YAML::Mark &m, YAML::anchor_t) override {
        Node n;
        n.kind = Node::Alias;
        setMark(n, m);
        add(std::move(n));
    }
    void OnScalar(const YAML::Mark &m, const std::string &tag, YAML::anchor_t a, const std::string &v) override {
        Node n;
        n.kind = Node::Scalar;
        setMark(n, m);
        n.tag = tag;
        n.anchored = a != YAML::NullAnchor;
        n.value = v;
        add(std::move(n));
    }
    void start(Node::Kind k, const YAML::Mark &m, const std::string &tag, YAML::anchor_t a, bool flow) {
        Node n;
        n.kind = k;
        n.flow = flow;
        setMark(n, m);
        n.tag = tag;
        n.anchored = a != YAML::NullAnchor;
        open.push_back(add(std::move(n)));
    }
    void OnSequenceStart(const YAML::Mark &m, const std::string &t, YAML::anchor_t a, YAML::EmitterStyle::value st) override {
        start(Node::Seq, m, t, a, st == YAML::EmitterStyle::Flow);
    }
    void OnSequenceEnd() override {
        if (!open.empty()) open.pop_back();
    }
    void OnMapStart(const YAML::Mark &m, const std::string &t, YAML::anchor_t a, YAML::EmitterStyle::value st) override {
        start(Node::Map, m, t, a, st == YAML::EmitterStyle::Flow);
    }
    void OnMapEnd() override {
        if (!open.empty()) open.pop_back();
    }
};

struct Walker {
    const std::vector<Node> &nodes;
    std::string_view text;
    const std::string &file;
    Layer &out;
    Diags &diags;
    bool ok = true;

    void error(const Node &n, std::string m) {
        Diag d;
        d.file = file;
        d.line = n.line;
        d.col = n.col;
        d.msg = std::move(m);
        diags.push_back(std::move(d));
        ok = false;
    }

    // yaml-cpp reports "!" for a quoted or block scalar without a tag, but an explicit
    // tag replaces that; then look at the source: skip the node's properties (tag,
    // anchor) and see whether the scalar starts with a quote or block indicator.
    bool quotedInSource(const Node &n) const {
        if (!n.hasPos) return false;
        std::size_t i = n.pos;
        while (i < text.size()) {
            const char c = text[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i;
            } else if (c == '!' || c == '&') {
                while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != '\r') ++i;
            } else {
                return c == '"' || c == '\'' || c == '|' || c == '>';
            }
        }
        return false;
    }

    static std::string displayTag(const std::string &t) {
        static const std::string core = "tag:yaml.org,2002:";
        if (t.rfind(core, 0) == 0) return "!!" + t.substr(core.size());
        return t;
    }

    void scalarInto(const Node &n, Value &v) {
        v.kind = Value::Scalar;
        v.scalar = n.value;
        if (n.tag == "!") {
            v.quoted = true;
        } else if (n.tag != "?" && !n.tag.empty()) {
            v.tag = displayTag(n.tag);
            v.quoted = quotedInSource(n);
        }
    }

    void map(const Node &m, const std::string &prefix, int depth) {
        if (depth > kMaxDepth) {
            error(m, "nesting too deep");
            return;
        }
        std::set<std::string> seen;
        for (std::size_t i = 0; i + 1 < m.kids.size(); i += 2) {
            const Node &kn = nodes[m.kids[i]];
            const Node &vn = nodes[m.kids[i + 1]];
            if (kn.kind == Node::Alias || vn.kind == Node::Alias || kn.anchored || vn.anchored) {
                error(kn, "anchors and aliases are not supported");
                continue;
            }
            if (kn.kind != Node::Scalar || kn.value.empty()) {
                error(kn, "expected a 'key: value' entry");
                continue;
            }
            const std::string &k = kn.value;
            if (k.find('.') != std::string::npos) {
                error(kn, "key '" + k + "' must not contain '.'; nest mappings instead");
                continue;
            }
            if (!seen.insert(k).second) {
                error(kn, "duplicate key '" + prefix + k + "'");
                continue;
            }
            const std::string full = prefix + k;
            Value v;
            v.file = file;
            v.line = kn.line;
            v.col = kn.col;
            if (vn.kind == Node::Map) {
                if (vn.kids.empty()) out.entries.emplace_back(full, std::move(v)); // "{}": treated as null
                else map(vn, full + ".", depth + 1);
            } else if (vn.kind == Node::Seq) {
                v.kind = Value::Seq;
                bool good = true;
                for (std::size_t it : vn.kids) {
                    const Node &in = nodes[it];
                    if (in.kind == Node::Null) {
                        v.items.emplace_back();
                    } else if (in.kind == Node::Scalar && !in.anchored) {
                        v.items.push_back(in.value);
                    } else {
                        error(in, "'" + full + "': list items must be plain values");
                        good = false;
                        break;
                    }
                }
                if (good) out.entries.emplace_back(full, std::move(v));
            } else {
                if (vn.kind == Node::Scalar) scalarInto(vn, v);
                out.entries.emplace_back(full, std::move(v));
            }
        }
    }
};

void addDiag(Diags &diags, const std::string &file, int line, int col, std::string msg) {
    Diag d;
    d.file = file;
    d.line = line;
    d.col = col;
    d.msg = std::move(msg);
    diags.push_back(std::move(d));
}

// yaml-cpp's parser recurses once per nesting level, so bound the input first.
// Flow brackets are counted everywhere (also inside quotes/comments): conservative,
// and a real settings file never nests 32 levels deep.
bool boundInput(std::string_view text, const std::string &file, Diags &diags) {
    if (text.size() > kMaxBytes) {
        addDiag(diags, file, 0, 0, "file too large (over " + std::to_string(kMaxBytes / 1024) + " KiB)");
        return false;
    }
    int depth = 0, line = 1, col = 0;
    for (char c : text) {
        ++col;
        if (c == '\n') line++, col = 0;
        else if (c == '[' || c == '{') {
            if (++depth > kMaxFlowNesting) {
                addDiag(diags, file, line, col, "nesting too deep");
                return false;
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return true;
}

} // namespace

bool parseYaml(std::string_view text, const std::string &file, Layer &out, Diags &diags) {
    const auto before = out.entries.size();
    if (!boundInput(text, file, diags)) return false;
    try {
        std::istringstream in{std::string(text)};
        YAML::Parser parser(in);
        Collector c;
        if (!parser.HandleNextDocument(c) || c.docs == 0) return true; // empty stream
        Collector extra;
        if (parser.HandleNextDocument(extra) && extra.docs > 0) {
            addDiag(diags, file, 0, 0, "only one YAML document is allowed");
            return false;
        }
        if (!c.haveRoot) return true;
        const Node &doc = c.nodes[c.root];
        Walker w{c.nodes, text, file, out, diags};
        if (doc.kind == Node::Map) {
            w.map(doc, "", 0);
        } else if (doc.kind == Node::Seq || doc.kind == Node::Alias || (doc.kind == Node::Scalar && !doc.value.empty())) {
            w.error(doc, "the top level must be a mapping of settings");
        } // else: empty document (only comments / "---")
        if (!w.ok) out.entries.resize(before);
        return w.ok;
    } catch (const YAML::Exception &e) {
        out.entries.resize(before);
        const int line = e.mark.is_null() || e.mark.line < 0 ? 0 : e.mark.line + 1;
        const int col = e.mark.is_null() || e.mark.column < 0 ? 0 : e.mark.column + 1;
        // yaml-cpp's own recursion guard (DepthGuard, 500 levels: block "- - - ..."
        // nesting, which the bracket pre-scan does not see) reports "bad file".
        if (dynamic_cast<const YAML::DeepRecursion *>(&e)) {
            addDiag(diags, file, line, col, "nesting too deep");
            return false;
        }
        std::string m = e.msg;
        if (const auto nl = m.find('\n'); nl != std::string::npos) m.resize(nl);
        addDiag(diags, file, line, col, "YAML syntax error: " + m);
        return false;
    } catch (const std::exception &e) {
        out.entries.resize(before);
        addDiag(diags, file, 0, 0, std::string("cannot parse YAML: ") + e.what());
        return false;
    }
}

// ---------------------------------------------------------------- in-place edits
//
// The UI writes settings back into cfg.yaml by replacing only the bytes of each value
// (or inserting one "key: value" line), so comments, blank lines, key order, quoting and
// indentation everywhere else stay exactly as the user wrote them. yaml-cpp reports
// where each scalar starts; its end is found in the source (end of line, before a
// " # comment", or the closing quote), and anything that is not a one-line value in a
// block mapping is refused rather than guessed at. Every edit is re-parsed and checked.

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

bool parseNodes(std::string_view text, Collector &c, std::string &err) {
    try {
        std::istringstream in{std::string(text)};
        YAML::Parser parser(in);
        parser.HandleNextDocument(c);
        return true;
    } catch (const std::exception &e) {
        err = std::string("cannot parse YAML: ") + e.what();
        return false;
    }
}

std::size_t lineStart(std::string_view t, std::size_t pos) {
    const auto nl = pos == 0 ? std::string_view::npos : t.rfind('\n', pos - 1);
    return nl == std::string_view::npos ? 0 : nl + 1;
}
std::size_t lineEnd(std::string_view t, std::size_t pos) { // index of '\n' or size
    const auto nl = t.find('\n', pos);
    return nl == std::string_view::npos ? t.size() : nl;
}

bool validKeySegment(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return s[0] != '-';
}

// A number, boolean or null in YAML 1.1/1.2 core terms: such text must stay plain.
bool looksTyped(const std::string &v) {
    static const char *words[] = {"true", "false", "yes", "no", "on", "off", "y", "n", "null", "~"};
    std::string l;
    for (char c : v) l += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    for (const char *w : words)
        if (l == w) return true;
    std::size_t i = (!v.empty() && (v[0] == '-' || v[0] == '+')) ? 1 : 0;
    if (i >= v.size()) return false;
    for (; i < v.size(); i++)
        if (v[i] < '0' || v[i] > '9') return false;
    return true;
}

bool plainSafe(const std::string &v) {
    if (v.empty() || isSpace(v.front()) || isSpace(v.back())) return false;
    if (std::string_view("-?:,[]{}#&*!|>'\"%@`").find(v[0]) != std::string_view::npos) {
        // "-5" and "-x" are plain; "- x", "-" are not.
        if (!(v[0] == '-' && v.size() > 1 && !isSpace(v[1]))) return false;
    }
    for (std::size_t i = 0; i < v.size(); i++) {
        const auto c = static_cast<unsigned char>(v[i]);
        if (c < 0x20 || c == 0x7f) return false;
        if (v[i] == ':' && (i + 1 == v.size() || isSpace(v[i + 1]))) return false;
        if (v[i] == '#' && i > 0 && isSpace(v[i - 1])) return false;
    }
    return true;
}

std::string doubleQuoted(const std::string &v) {
    std::string r = "\"";
    for (char ch : v) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') r += '\\', r += ch;
        else if (ch == '\n') r += "\\n";
        else if (ch == '\t') r += "\\t";
        else if (c < 0x20 || c == 0x7f) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            r += b;
        } else r += ch;
    }
    return r + "\"";
}

bool isNullWord(const std::string &v) { return v == "~" || v == "null" || v == "Null" || v == "NULL"; }

// `quote` is the original value's quote character ('\'' or '"'), or 0 if it was plain.
// Numbers and booleans are written plain; a null word ("null", "~") is a string here
// and is always quoted (plain it would read back as no value).
std::string formatScalar(const std::string &v, char quote) {
    if (isNullWord(v) && !quote) quote = '"';
    if (quote && (!looksTyped(v) || isNullWord(v))) {
        if (quote == '\'') {
            bool ok = true;
            for (char ch : v)
                if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) ok = false;
            if (ok) {
                std::string r = "'";
                for (char ch : v) r += ch == '\'' ? std::string("''") : std::string(1, ch);
                return r + "'";
            }
        }
        return doubleQuoted(v);
    }
    if (plainSafe(v)) return v;
    return doubleQuoted(v);
}

std::string diagText(const Diags &d) {
    if (d.empty()) return "unknown error";
    const Diag &x = d.front();
    return (x.line > 0 ? "line " + std::to_string(x.line) + ": " : std::string()) + x.msg;
}

bool sameValue(const Value &a, const Value &b) {
    return a.kind == b.kind && a.scalar == b.scalar && a.items == b.items && a.quoted == b.quoted && a.tag == b.tag;
}

// One edit: `segs` (dotted key split) = `value`.
bool editOne(const std::string &text, const std::vector<std::string> &segs, const std::string &value, std::string &out,
             std::string &err) {
    const std::string dotted = [&] {
        std::string k;
        for (const auto &s : segs) k += (k.empty() ? "" : ".") + s;
        return k;
    }();
    Collector c;
    if (!parseNodes(text, c, err)) return false;
    const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const auto &N = c.nodes;
    auto col = [&](const Node &n) { return n.pos - lineStart(text, n.pos); };

    // Indentation step: from the first nested block mapping, else 2.
    std::size_t step = 2;
    if (c.haveRoot && N[c.root].kind == Node::Map && !N[c.root].flow) {
        const Node &r = N[c.root];
        for (std::size_t i = 0; i + 1 < r.kids.size(); i += 2) {
            const Node &k = N[r.kids[i]], &v = N[r.kids[i + 1]];
            if (v.kind == Node::Map && !v.flow && !v.kids.empty() && k.hasPos && N[v.kids[0]].hasPos &&
                col(N[v.kids[0]]) > col(k)) {
                step = col(N[v.kids[0]]) - col(k);
                break;
            }
        }
    }
    // "a:\n  b:\n    c: value\n" for segs[from..] at `indent`.
    auto block = [&](std::size_t from, std::size_t indent) {
        std::string b;
        for (std::size_t i = from; i < segs.size(); i++, indent += step) {
            b += std::string(indent, ' ') + segs[i] + ":";
            b += i + 1 == segs.size() ? " " + formatScalar(value, 0) + nl : nl;
        }
        return b;
    };
    // Inserts `b` as new line(s) starting at `at` (a line start, or text.size()).
    auto insertLines = [&](std::size_t at, const std::string &b) {
        out = text.substr(0, at);
        if (!out.empty() && out.back() != '\n') out += nl;
        out += b;
        out += text.substr(at);
        return true;
    };

    if (!c.haveRoot || N[c.root].kind == Node::Null ||
        (N[c.root].kind == Node::Scalar && N[c.root].value.empty())) // empty / comments only
        return insertLines(text.size(), block(0, 0));
    if (N[c.root].kind != Node::Map) {
        err = "the top level is not a mapping of settings";
        return false;
    }
    std::size_t cur = c.root;
    for (std::size_t si = 0; si < segs.size(); si++) {
        const Node &m = N[cur];
        const std::string here = [&] {
            std::string k;
            for (std::size_t i = 0; i <= si; i++) k += (i ? "." : "") + segs[i];
            return k;
        }();
        if (m.flow) {
            err = "'" + (si ? here.substr(0, here.rfind('.')) : std::string("the top level")) +
                  "' is a flow mapping ({...}); it is not rewritten, edit it by hand or write it as indented lines";
            return false;
        }
        std::size_t ki = SIZE_MAX;
        for (std::size_t i = 0; i + 1 < m.kids.size(); i += 2)
            if (N[m.kids[i]].kind == Node::Scalar && N[m.kids[i]].value == segs[si]) ki = i;
        if (ki == SIZE_MAX) {
            // Append "key: value" after the last line of this mapping: its last node, then
            // any following lines indented at least as deep as its keys (continuations,
            // comments that belong to it); blank lines in between are skipped.
            if (cur == c.root) return insertLines(text.size(), block(si, 0));
            if (m.kids.empty() || !N[m.kids[0]].hasPos) {
                err = "cannot locate '" + here + "' in the file";
                return false;
            }
            const std::size_t indent = col(N[m.kids[0]]);
            std::size_t maxPos = 0;
            std::vector<std::size_t> stack{cur};
            while (!stack.empty()) {
                const Node &n = N[stack.back()];
                stack.pop_back();
                if (n.hasPos) maxPos = std::max(maxPos, n.pos);
                for (auto k : n.kids) stack.push_back(k);
            }
            std::size_t after = lineEnd(text, maxPos); // insert after this line
            for (std::size_t p = after; p < text.size();) {
                const std::size_t ls = p + 1;
                if (ls >= text.size()) break;
                const std::size_t le = lineEnd(text, ls);
                std::size_t i = ls;
                while (i < le && text[i] == ' ') ++i;
                const bool blank = i == le || (text[i] == '\r' && i + 1 == le);
                if (!blank) {
                    if (i - ls < indent || text[i] == '\t') break;
                    after = le;
                }
                p = le;
            }
            return insertLines(after < text.size() ? after + 1 : text.size(), block(si, indent));
        }
        const Node &kn = N[m.kids[ki]], &vn = N[m.kids[ki + 1]];
        if (!kn.hasPos || text.compare(kn.pos, segs[si].size(), segs[si]) != 0) {
            err = "the key of '" + here + "' (line " + std::to_string(kn.line) +
                  ") is quoted or written in an unusual form; it is not rewritten";
            return false;
        }
        std::size_t colon = kn.pos + segs[si].size();
        while (colon < text.size() && isSpace(text[colon])) ++colon;
        if (colon >= text.size() || text[colon] != ':') {
            err = "cannot find the ':' after '" + here + "' (line " + std::to_string(kn.line) + ")";
            return false;
        }
        const std::size_t le = lineEnd(text, colon);
        const std::size_t ce = (le > colon && text[le - 1] == '\r') ? le - 1 : le; // content end
        std::size_t vs = colon + 1;
        while (vs < ce && isSpace(text[vs])) ++vs;
        const bool emptyHere = vs == ce || text[vs] == '#';
        const std::string where = "'" + here + "' (line " + std::to_string(kn.line) + ")";
        if (si + 1 < segs.size()) { // a section on the way down
            if (vn.kind == Node::Map) {
                cur = m.kids[ki + 1];
                continue;
            }
            if (vn.kind == Node::Null && emptyHere) // "cache:" with nothing under it
                return insertLines(le < text.size() ? le + 1 : text.size(), block(si + 1, col(kn) + step));
            err = where + " is a value, not a section; cannot set '" + dotted + "' under it";
            return false;
        }
        // The value itself.
        if ((vn.kind == Node::Map || vn.kind == Node::Seq) && vn.flow) {
            err = where + " is a flow collection ([...] or {...}); it is not rewritten";
            return false;
        }
        if (vn.kind == Node::Map || vn.kind == Node::Seq) {
            err = where + " is a " + std::string(vn.kind == Node::Map ? "mapping" : "list") +
                  ", not a single value; it is not rewritten";
            return false;
        }
        if (emptyHere) {
            if (vn.kind != Node::Null) {
                err = where + ": the value is on a following line; it is not rewritten";
                return false;
            }
            out = text.substr(0, colon + 1) + " " + formatScalar(value, 0) + text.substr(colon + 1);
            return true;
        }
        const char c0 = text[vs];
        if (c0 == '[' || c0 == '{') {
            err = where + " is a flow collection ([...] or {...}); it is not rewritten";
            return false;
        }
        if (c0 == '|' || c0 == '>') {
            err = where + " is a multi-line block scalar (| or >); it is not rewritten";
            return false;
        }
        if (c0 == '&' || c0 == '*') {
            err = where + " uses an anchor or alias; it is not rewritten";
            return false;
        }
        if (c0 == '!') {
            err = where + " has an explicit tag (e.g. !!int); it is not rewritten";
            return false;
        }
        std::size_t ve = vs;
        char quote = 0;
        if (c0 == '"' || c0 == '\'') {
            quote = c0;
            std::size_t i = vs + 1;
            bool closed = false;
            while (i < ce) {
                if (c0 == '"' && text[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (text[i] == c0) {
                    if (c0 == '\'' && i + 1 < ce && text[i + 1] == '\'') {
                        i += 2;
                        continue;
                    }
                    closed = true;
                    break;
                }
                ++i;
            }
            if (!closed) {
                err = where + " is a quoted value spanning several lines; it is not rewritten";
                return false;
            }
            ve = i + 1;
            std::size_t j = ve;
            while (j < ce && isSpace(text[j])) ++j;
            if (j < ce && text[j] != '#') {
                err = "cannot find the end of " + where;
                return false;
            }
        } else {
            std::size_t i = vs;
            while (i < ce && !(text[i] == '#' && isSpace(text[i - 1]))) ++i;
            ve = i;
            while (ve > vs && isSpace(text[ve - 1])) --ve;
            const std::string_view span(text.data() + vs, ve - vs);
            const bool match = vn.kind == Node::Scalar ? (vn.value == span && vn.tag == "?")
                                                       : (span == "~" || span == "null" || span == "Null" || span == "NULL");
            if (!match) {
                err = where + " is a value spanning several lines; it is not rewritten";
                return false;
            }
        }
        if (vn.kind == Node::Scalar && (!vn.hasPos || vn.pos != vs)) {
            err = "cannot locate the value of " + where;
            return false;
        }
        out = text.substr(0, vs) + formatScalar(value, quote) + text.substr(ve);
        return true;
    }
    err = "cannot set '" + dotted + "'";
    return false;
}

} // namespace

bool editYamlValues(std::string_view text, const std::vector<std::pair<std::string, std::string>> &values,
                    std::string &out, std::string &err) {
    // A UTF-8 byte order mark (some Windows editors add one) is set aside while editing:
    // yaml-cpp's marks do not count it, so every position would be 3 bytes off.
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    const bool hasBom = text.substr(0, bom.size()) == bom;
    std::string cur(hasBom ? text.substr(bom.size()) : text);
    for (const auto &[key, value] : values) {
        std::vector<std::string> segs;
        for (std::size_t p = 0;;) {
            const auto d = key.find('.', p);
            segs.push_back(key.substr(p, d == std::string::npos ? std::string::npos : d - p));
            if (d == std::string::npos) break;
            p = d + 1;
        }
        for (const auto &s : segs)
            if (!validKeySegment(s)) {
                err = "invalid key '" + key + "'";
                return false;
            }
        Layer before;
        Diags d;
        if (!parseYaml(cur, "", before, d)) {
            err = "the file has errors, not rewriting it: " + diagText(d);
            return false;
        }
        std::string next;
        if (!editOne(cur, segs, value, next, err)) return false;
        // Round trip: the edited key now reads back as `value`, everything else as before
        // (except an empty parent section "cache:" that now has the key under it).
        Layer after;
        Diags d2;
        if (!parseYaml(next, "", after, d2)) {
            err = "internal error: the edited file does not parse (" + diagText(d2) +
                  "); not written";
            return false;
        }
        std::map<std::string, const Value *> a;
        for (const auto &[k, v] : after.entries) a[k] = &v;
        bool ok = a.count(key) && a[key]->kind == Value::Scalar && a[key]->scalar == value && a[key]->tag.empty();
        std::size_t expect = 0;
        for (const auto &[k, v] : before.entries) {
            if (k == key) continue;
            if (v.kind == Value::Null && key.size() > k.size() && key.compare(0, k.size(), k) == 0 && key[k.size()] == '.')
                continue; // "cache:" became "cache:\n  x: v"
            ++expect;
            ok = ok && a.count(k) && sameValue(*a[k], v);
        }
        ok = ok && after.entries.size() == expect + 1;
        if (!ok) {
            err = "internal error: setting '" + key + "' did not round-trip; not written";
            return false;
        }
        cur = std::move(next);
    }
    out = hasBom ? std::string(bom) + cur : std::move(cur);
    return true;
}

} // namespace lm::config
