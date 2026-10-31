// YAML -> Layer with yaml-cpp (system library). The only file that sees yaml-cpp types.
//
// We use yaml-cpp's event API (YAML::Parser + EventHandler), not YAML::Node: Node
// silently merges duplicate keys and resolves aliases, and we need to reject both. The
// events are collected into a flat node table (no recursion, so no deep destructor
// chains), then walked with a bounded depth.
#include "lm/config/config.hpp"

#include <yaml-cpp/depthguard.h>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/mark.h>
#include <yaml-cpp/parser.h>

#include <cstddef>
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
    void start(Node::Kind k, const YAML::Mark &m, const std::string &tag, YAML::anchor_t a) {
        Node n;
        n.kind = k;
        setMark(n, m);
        n.tag = tag;
        n.anchored = a != YAML::NullAnchor;
        open.push_back(add(std::move(n)));
    }
    void OnSequenceStart(const YAML::Mark &m, const std::string &t, YAML::anchor_t a, YAML::EmitterStyle::value) override {
        start(Node::Seq, m, t, a);
    }
    void OnSequenceEnd() override {
        if (!open.empty()) open.pop_back();
    }
    void OnMapStart(const YAML::Mark &m, const std::string &t, YAML::anchor_t a, YAML::EmitterStyle::value) override {
        start(Node::Map, m, t, a);
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

} // namespace lm::config
