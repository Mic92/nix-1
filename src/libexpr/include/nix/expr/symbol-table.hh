#pragma once
///@file

#include <list>
#include <map>
#include <unordered_map>

#include "nix/util/types.hh"
#include "nix/util/chunked-vector.hh"
#include "nix/util/error.hh"

namespace nix {

/**
 * This class mainly exists to give us an operator<< for ostreams. We could also
 * return plain strings from SymbolTable, but then we'd have to wrap every
 * instance of a symbol that is fmt()ed, which is inconvenient and error-prone.
 */
class SymbolStr
{
    friend class SymbolTable;

private:
    const std::string * s;

    explicit SymbolStr(const std::string & symbol)
        : s(&symbol)
    {
    }

public:
    bool operator==(std::string_view s2) const
    {
        return *s == s2;
    }

    const char * c_str() const
    {
        return s->c_str();
    }

    operator const std::string_view() const
    {
        return *s;
    }

    friend std::ostream & operator<<(std::ostream & os, const SymbolStr & symbol);

    bool empty() const
    {
        return s->empty();
    }
};

/**
 * Symbols have the property that they can be compared efficiently
 * (using an equality test), because the symbol table stores only one
 * copy of each string.
 */
class Symbol
{
    friend class SymbolTable;

private:
    uint32_t id;

    explicit Symbol(uint32_t id)
        : id(id)
    {
    }

public:
    Symbol()
        : id(0)
    {
    }

    explicit operator bool() const
    {
        return id > 0;
    }

    auto operator<=>(const Symbol other) const
    {
        return id <=> other.id;
    }
    bool operator==(const Symbol other) const
    {
        return id == other.id;
    }

    friend class std::hash<Symbol>;
};

/**
 * Symbol table used by the parser and evaluator to represent and look
 * up identifiers and attributes efficiently.
 */
class SymbolTable
{
private:
    /**
     * Map from string view (backed by ChunkedVector) -> offset into the store.
     * ChunkedVector references are never invalidated.
     */
    std::unordered_map<std::string_view, uint32_t> symbols;
    ChunkedVector<std::string, 8192> store{16};

public:

    /**
     * Converts a string into a symbol.
     */
    Symbol create(std::string_view s)
    {
        // Most symbols are looked up more than once, so we trade off insertion performance
        // for lookup performance.
        // FIXME: make this thread-safe.
        auto it = symbols.find(s);
        if (it != symbols.end())
            return Symbol(it->second + 1);

        const auto & [rawSym, idx] = store.add(s);
        symbols.emplace(rawSym, idx);
        return Symbol(idx + 1);
    }

    std::vector<SymbolStr> resolve(const std::vector<Symbol> & symbols) const
    {
        std::vector<SymbolStr> result;
        result.reserve(symbols.size());
        for (auto sym : symbols)
            result.push_back((*this)[sym]);
        return result;
    }

    SymbolStr operator[](Symbol s) const
    {
        if (s.id == 0 || s.id > store.size())
            unreachable();
        return SymbolStr(store[s.id - 1]);
    }

    size_t size() const
    {
        return store.size();
    }

    size_t totalSize() const;

    template<typename T>
    void dump(T callback) const
    {
        store.forEach(callback);
    }
};

}

template<>
struct std::hash<nix::Symbol>
{
    std::size_t operator()(const nix::Symbol & s) const noexcept
    {
        return std::hash<decltype(s.id)>{}(s.id);
    }
};
