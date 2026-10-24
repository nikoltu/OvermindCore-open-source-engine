#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace sc2::core {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;

    const std::string* attr(const std::string& key) const {
        for (const auto& [k, v] : attrs)
            if (k == key) return &v;
        return nullptr;
    }
    const std::string& attr_or(const std::string& key,
                               const std::string& fallback) const {
        const std::string* v = attr(key);
        return v ? *v : fallback;
    }
    std::vector<const XmlNode*> elements(const std::string& tag) const {
        std::vector<const XmlNode*> out;
        for (const auto& c : children)
            if (c.name == tag) out.push_back(&c);
        return out;
    }
};

class XmlNodeList {
public:
    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = XmlNode;
        using difference_type = std::ptrdiff_t;
        using pointer = const XmlNode*;
        using reference = const XmlNode&;
        const_iterator() = default;
        explicit const_iterator(const XmlNode* const* p) : p_(p) {}
        reference operator*() const { return **p_; }
        pointer operator->() const { return *p_; }
        reference operator[](difference_type n) const { return *p_[n]; }
        const_iterator& operator++() { ++p_; return *this; }
        const_iterator operator++(int) { const_iterator t = *this; ++p_; return t; }
        const_iterator& operator--() { --p_; return *this; }
        const_iterator operator--(int) { const_iterator t = *this; --p_; return t; }
        const_iterator& operator+=(difference_type n) { p_ += n; return *this; }
        const_iterator& operator-=(difference_type n) { p_ -= n; return *this; }
        friend const_iterator operator+(const_iterator a, difference_type n) { return a += n; }
        friend const_iterator operator+(difference_type n, const_iterator a) { return a += n; }
        friend const_iterator operator-(const_iterator a, difference_type n) { return a -= n; }
        friend difference_type operator-(const const_iterator& a, const const_iterator& b) { return a.p_ - b.p_; }
        friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.p_ == b.p_; }
        friend bool operator!=(const const_iterator& a, const const_iterator& b) { return a.p_ != b.p_; }
        friend bool operator<(const const_iterator& a, const const_iterator& b) { return a.p_ < b.p_; }
        friend bool operator>(const const_iterator& a, const const_iterator& b) { return a.p_ > b.p_; }
        friend bool operator<=(const const_iterator& a, const const_iterator& b) { return a.p_ <= b.p_; }
        friend bool operator>=(const const_iterator& a, const const_iterator& b) { return a.p_ >= b.p_; }

    private:
        const XmlNode* const* p_ = nullptr;
    };
    using iterator = const_iterator;
    using value_type = XmlNode;
    using size_type = std::size_t;
    using const_reference = const XmlNode&;
    using reference = const XmlNode&;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    const_iterator begin() const { return const_iterator(p_.data()); }
    const_iterator end() const { return const_iterator(p_.data() + p_.size()); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    std::size_t size() const { return p_.size(); }
    bool empty() const { return p_.empty(); }
    const XmlNode& operator[](std::size_t i) const { return *p_[i]; }
    const XmlNode& front() const { return *p_.front(); }
    const XmlNode& back() const { return *p_.back(); }
    void push_back(const XmlNode* n) { p_.push_back(n); }
    void append(const XmlNodeList& o) { p_.insert(p_.end(), o.p_.begin(), o.p_.end()); }
    void reserve(std::size_t n) { p_.reserve(n); }
    std::size_t memory_bytes() const { return p_.capacity() * sizeof(const XmlNode*); }
    std::vector<XmlNode> to_vector() const {
        std::vector<XmlNode> v;
        v.reserve(p_.size());
        for (const XmlNode* n : p_) v.push_back(*n);
        return v;
    }

private:
    std::vector<const XmlNode*> p_;
};

std::size_t xml_node_heap_bytes(const XmlNode& n);

XmlNode xml_parse(const std::string& data);
XmlNode xml_parse(const std::vector<uint8_t>& data);

}
