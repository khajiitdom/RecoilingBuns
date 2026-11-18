// Recoiling Buns: vertex layout math for the runtime colour layer. Header-only and free of game types, so the same code
// also runs outside the game.
//
// The game describes a vertex with a 64-bit descriptor (BSGraphics::VertexDesc):
//   bits  0- 3   vertex size / 4
//   bits  4-43   offset / 4 of attribute a in nibble a + 1 (a = 0..9)
//   bits 44-55   flags: attribute a present at bit 44 + a, full-precision positions at bit 54
// A mesh without vertex colours cannot show the ripple's shading. AliasColour (what the plugin uses) gives the same
// layout with the colour in the full-precision position's unused w: nothing moves, nothing grows. WithColour (a colour
// attribute inserted where the game's own meshes carry one, the vertex 4 bytes longer) and Expand / Collapse (vertex
// data between the two layouts) are an older way to add the colour layer, not used by the plugin; kept for reference.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace BR
{
    // Vertex attributes, in the game's order (BSGraphics::Vertex::Attribute).
    namespace Attribute
    {
        inline constexpr int kPosition = 0;
        inline constexpr int kUV = 1;
        inline constexpr int kUV2 = 2;
        inline constexpr int kNormal = 3;
        inline constexpr int kTangent = 4;   // the game's "binormal"
        inline constexpr int kColour = 5;
        inline constexpr int kSkinning = 6;
        inline constexpr int kLandData = 7;
        inline constexpr int kEyeData = 8;
        inline constexpr int kInstanceData = 9;
        inline constexpr int kCount = 10;
    }

    // The 4 bytes of one vertex colour (r, g, b, a).
    inline constexpr std::uint32_t kColourSize = 4;
    // The largest vertex the descriptor can describe (its size nibble counts dwords).
    inline constexpr std::uint32_t kMaxStride = 60;

    // A vertex layout with a colour attribute: its descriptor, vertex size and colour offset (bytes).
    struct ColourLayout
    {
        std::uint64_t desc = 0;
        std::uint32_t stride = 0, colourOffset = 0;
    };

    namespace detail
    {
        inline std::uint32_t Nibble(std::uint64_t a_desc, int a_index) { return static_cast<std::uint32_t>((a_desc >> (4 * a_index)) & 0xF); }

        inline std::uint64_t WithNibble(std::uint64_t a_desc, int a_index, std::uint32_t a_value)
        {
            const int shift = 4 * a_index;
            return (a_desc & ~(std::uint64_t{ 0xF } << shift)) | (std::uint64_t{ a_value & 0xF } << shift);
        }
    }

    // Vertex size in bytes.
    inline std::uint32_t StrideOf(std::uint64_t a_desc) { return detail::Nibble(a_desc, 0) * 4; }

    // Byte offset of an attribute inside the vertex (meaningful only when the attribute is present).
    inline std::uint32_t OffsetOf(std::uint64_t a_desc, int a_attribute)
    {
        if (a_attribute < 0 || a_attribute >= Attribute::kCount) return 0;
        return detail::Nibble(a_desc, a_attribute + 1) * 4;
    }

    inline bool HasAttribute(std::uint64_t a_desc, int a_attribute)
    {
        if (a_attribute < 0 || a_attribute >= Attribute::kCount) return false;
        return ((a_desc >> (44 + a_attribute)) & 1) != 0;
    }

    // a_desc with a colour attribute inserted where the game's own meshes carry it: after position, uvs, normal and
    // tangent, before skinning, land, eye and instance data. Those move 4 bytes on, the vertex grows by 4 bytes and the
    // colour flag is set; every other bit is kept. nullopt when the layout has colours already, would pass 60 bytes,
    // has no attribute, or its present attributes do not sit inside the vertex in attribute order: a layout we do not
    // recognise is left alone.
    inline std::optional<ColourLayout> WithColour(std::uint64_t a_desc)
    {
        if (HasAttribute(a_desc, Attribute::kColour)) return std::nullopt;
        const std::uint32_t stride = StrideOf(a_desc);
        if (stride == 0 || stride + kColourSize > kMaxStride) return std::nullopt;

        std::uint32_t colourOffset = stride;   // the end of the vertex unless an attribute follows the colour
        bool          any = false;
        std::uint32_t previous = 0;
        for (int a = 0; a < Attribute::kCount; ++a) {
            if (!HasAttribute(a_desc, a)) continue;
            const std::uint32_t offset = OffsetOf(a_desc, a);
            if (offset >= stride || (any && offset <= previous)) return std::nullopt;
            if (a > Attribute::kColour && colourOffset == stride) colourOffset = offset;
            previous = offset;
            any = true;
        }
        if (!any) return std::nullopt;

        std::uint64_t desc = detail::WithNibble(a_desc, 0, (stride + kColourSize) / 4);
        desc = detail::WithNibble(desc, Attribute::kColour + 1, colourOffset / 4);
        for (int a = Attribute::kColour + 1; a < Attribute::kCount; ++a)
            if (HasAttribute(a_desc, a)) desc = detail::WithNibble(desc, a + 1, (OffsetOf(a_desc, a) + kColourSize) / 4);
        desc |= std::uint64_t{ 1 } << (44 + Attribute::kColour);
        return ColourLayout{ desc, stride + kColourSize, colourOffset };
    }

    // The colour layer without a new layout (1.2): a full-precision position is 16 bytes - x, y, z and a w that the
    // skinned model-space-normal vertex shaders never read (every one of them reads POSITION.xyz only; w carries the
    // bitangent only in tangent-space meshes, whose shaders also need normals). The colour can sit in those 4 bytes:
    // the vertex keeps its size and every attribute stays where it is, only the colour flag and the colour offset (12)
    // are added. The vertex data, the skin partition's layout and the geometry's layout are never touched, so a copy of
    // the mesh (RaceMenu's skin partition copy reads exactly those) can never see it. nullopt when the layout has
    // colours already, its position is not at offset 0, its vertex is shorter than 16 bytes, or another attribute sits
    // in bytes 12..15 (a half-precision position: the caller also checks the position is full precision).
    inline std::optional<ColourLayout> AliasColour(std::uint64_t a_desc)
    {
        constexpr std::uint32_t kAt = 12;
        if (HasAttribute(a_desc, Attribute::kColour)) return std::nullopt;
        if (!HasAttribute(a_desc, Attribute::kPosition) || OffsetOf(a_desc, Attribute::kPosition) != 0) return std::nullopt;
        const std::uint32_t stride = StrideOf(a_desc);
        if (stride < kAt + kColourSize) return std::nullopt;
        for (int a = Attribute::kPosition + 1; a < Attribute::kCount; ++a) {
            if (!HasAttribute(a_desc, a)) continue;
            const std::uint32_t offset = OffsetOf(a_desc, a);
            if (offset < kAt + kColourSize || offset >= stride) return std::nullopt;   // inside the position or past the vertex
        }
        std::uint64_t desc = detail::WithNibble(a_desc, Attribute::kColour + 1, kAt / 4);
        desc |= std::uint64_t{ 1 } << (44 + Attribute::kColour);
        return ColourLayout{ desc, stride, kAt };
    }

    // a_count vertices of a_srcStride bytes -> the colour layout a_to (a_to.stride bytes each): the bytes before the
    // colour offset stay in place, the colour is FF FF FF FF (white, opaque), the rest follows it. Any byte of a_to's
    // vertex that the source does not fill is zero.
    inline void Expand(const std::uint8_t* a_src, std::uint32_t a_count, std::uint32_t a_srcStride, const ColourLayout& a_to, std::uint8_t* a_dst)
    {
        const std::uint32_t colour = (std::min)(a_to.colourOffset, a_to.stride);
        const std::uint32_t head = (std::min)(colour, a_srcStride);
        const std::uint32_t colourBytes = (std::min)(kColourSize, a_to.stride - colour);
        const std::uint32_t tailAt = colour + colourBytes;
        const std::uint32_t tail = (std::min)(a_srcStride - head, a_to.stride - tailAt);
        const bool          gaps = head < colour || tailAt + tail < a_to.stride;   // only for mismatched strides
        for (std::uint32_t i = 0; i < a_count; ++i) {
            const std::uint8_t* s = a_src + std::size_t{ i } * a_srcStride;
            std::uint8_t*       d = a_dst + std::size_t{ i } * a_to.stride;
            if (gaps) std::memset(d, 0, a_to.stride);
            std::memcpy(d, s, head);
            std::memset(d + colour, 0xFF, colourBytes);
            std::memcpy(d + tailAt, s + head, tail);
        }
    }

    // The inverse: a_count vertices of the colour layout a_from -> a_dstStride bytes each, every byte but the colour.
    // Collapse(Expand(x)) == x byte for byte. Any byte of the destination vertex the source does not fill is zero.
    inline void Collapse(const std::uint8_t* a_src, std::uint32_t a_count, const ColourLayout& a_from, std::uint32_t a_dstStride, std::uint8_t* a_dst)
    {
        const std::uint32_t colour = (std::min)(a_from.colourOffset, a_from.stride);
        const std::uint32_t head = (std::min)(colour, a_dstStride);
        const std::uint32_t tailAt = colour + (std::min)(kColourSize, a_from.stride - colour);
        const std::uint32_t tail = (std::min)(a_from.stride - tailAt, a_dstStride - head);
        const bool          gaps = head + tail < a_dstStride;   // only for mismatched strides
        for (std::uint32_t i = 0; i < a_count; ++i) {
            const std::uint8_t* s = a_src + std::size_t{ i } * a_from.stride;
            std::uint8_t*       d = a_dst + std::size_t{ i } * a_dstStride;
            if (gaps) std::memset(d, 0, a_dstStride);
            std::memcpy(d, s, head);
            std::memcpy(d + head, s + tailAt, tail);
        }
    }
}
