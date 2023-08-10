#include <bit>
#include <bitset>
#include <cassert>
#include <compatibility.hxx>
#include <functional>
#include <iostream>
#include <log.hxx>
#include <n64/core/n64_addresses.hxx>
#include <n64/core/n64_rdp.hxx>
#include <n64/core/n64_rdp_commands.hxx>
#include <sstream>
#include <str_hash.hxx>

static inline uint32_t rgba16_to_rgba32(uint16_t color)
{
    uint8_t r16 = (color >> 11) & 0x1F;
    uint8_t g16 = (color >> 6) & 0x1F;
    uint8_t b16 = (color >> 1) & 0x1F;
    uint8_t r = (r16 << 3) | (r16 >> 2);
    uint8_t g = (g16 << 3) | (g16 >> 2);
    uint8_t b = (b16 << 3) | (b16 >> 2);
    uint8_t a = (color & 0x1) ? 0xFF : 0x00;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static inline uint16_t rgba32_to_rgba16(uint32_t color)
{
    uint8_t r = (color >> 3) & 0x1F;
    uint8_t g = (color >> 11) & 0x1F;
    uint8_t b = (color >> 19) & 0x1F;
    uint8_t a = (color >> 24) & 0x1;
    return (r << 11) | (g << 6) | (b << 1) | a;
}

namespace hydra::N64
{
    constexpr inline std::string_view get_rdp_command_name(RDPCommandType type)
    {
        switch (type)
        {
#define X(name, opcode, length) \
    case RDPCommandType::name:  \
        return #name;
            RDP_COMMANDS
#undef X
            default:
                return "Unknown";
        }
    }

    constexpr inline int get_rdp_command_length(RDPCommandType type)
    {
        switch (type)
        {
#define X(name, opcodes, length) \
    case RDPCommandType::name:   \
        return length;
            RDP_COMMANDS
#undef X
            default:
                Logger::Warn("Unknown command length for command: {} ({:02x})",
                             get_rdp_command_name(type), static_cast<int>(type));
                return 1;
        }
    }

    RDP::RDP()
    {
        rdram_9th_bit_.resize(0x800000);
        init_depth_luts();
        Reset();
    }

    void RDP::InstallBuses(uint8_t* rdram_ptr, uint8_t* spmem_ptr)
    {
        rdram_ptr_ = rdram_ptr;
        spmem_ptr_ = spmem_ptr;
    }

    uint32_t RDP::ReadWord(uint32_t addr)
    {
        switch (addr)
        {
            case DP_START:
                return start_address_;
            case DP_END:
                return end_address_;
            case DP_STATUS:
                return status_.full;
            case DP_CLOCK:
                return 0; // ???
            case DP_BUSY:
                return 0; // ???
            default:
            {
                Logger::Warn("RDP: Unhandled read from {:08X}", addr);
                return 0;
            }
        }
    }

    void RDP::WriteWord(uint32_t addr, uint32_t data)
    {
        switch (addr)
        {
            case DP_START:
            {
                if (!status_.start_pending)
                {
                    start_address_ = data & 0xFFFFF8;
                }
                status_.start_pending = 1;
                break;
            }
            case DP_END:
            {
                if (status_.start_pending)
                {
                    // New transfer
                    status_.start_pending = 0;
                    // TODO: test if we can safely uncomment or if is correct
                    // status_.dma_busy = 1;
                    current_address_ = start_address_;
                }
                end_address_ = data & 0xFFFFF8;
                status_.pipe_busy = 1;
                status_.start_gclk = 1;
                process_commands();
                status_.ready = 1;
                break;
            }
            case DP_STATUS:
            {
                RDPStatusWrite write;
                write.full = data;
#define flag(x)                                 \
    if (write.clear_##x && !write.set_##x)      \
        status_.x = 0;                          \
    else if (write.set_##x && !write.clear_##x) \
        status_.x = 1;
                flag(dma_source_dmem);
                flag(freeze);
                flag(flush);
                if (write.clear_tmem_busy)
                {
                    status_.tmem_busy = 0;
                }
                if (write.clear_pipe_busy)
                {
                    status_.pipe_busy = 0;
                }
                if (write.clear_buffer_busy)
                {
                    status_.cmd_busy = 0;
                }
#undef flag
                break;
            }
        }
    }

    void RDP::Reset()
    {
        status_.ready = 1;
        color_sub_a_ = &color_one_;
        color_sub_b_ = &color_zero_;
        color_multiplier_ = &color_one_;
        color_adder_ = &color_zero_;
        alpha_sub_a_ = &color_zero_;
        alpha_sub_b_ = &color_zero_;
        alpha_multiplier_ = &color_one_;
        alpha_adder_ = &color_zero_;
        blender_1a_0_ = 0;
        blender_1b_0_ = 0;
        blender_2a_0_ = 0;
        blender_2b_0_ = 0;
        texel_color_0_ = 0xFFFFFFFF;
        texel_color_1_ = 0xFFFFFFFF;
        texel_alpha_0_ = 0xFFFFFFFF;
        texel_alpha_1_ = 0xFFFFFFFF;
    }

    void RDP::SendCommand(const std::vector<uint64_t>& data)
    {
        execute_command(data);
    }

    void RDP::process_commands()
    {
        uint32_t current = current_address_ & 0xFFFFF8;
        uint32_t end = end_address_ & 0xFFFFF8;

        status_.freeze = 1;
        while (current < end)
        {
            uintptr_t address = status_.dma_source_dmem ? reinterpret_cast<uintptr_t>(spmem_ptr_)
                                                        : reinterpret_cast<uintptr_t>(rdram_ptr_);
            uint64_t data = hydra::bswap64(*reinterpret_cast<uint64_t*>(address + current));
            uint8_t command_type = (data >> 56) & 0b111111;

            if (command_type >= 8)
            {
                int length = get_rdp_command_length(static_cast<RDPCommandType>(command_type));
                std::vector<uint64_t> command;
                command.resize(length);
                for (int i = 0; i < length; i++)
                {
                    command[i] =
                        hydra::bswap64(*reinterpret_cast<uint64_t*>(address + current + (i * 8)));
                }
                execute_command(command);
                // Logger::Info("RDP: Command {} ({:02x})",
                // get_rdp_command_name(static_cast<RDPCommandType>(command_type)),
                // static_cast<int>(command_type));
                current += length * 8;
            }
            else
            {
                current += 8;
            }
        }

        current_address_ = end_address_;
        status_.freeze = 0;
    }

    void RDP::execute_command(const std::vector<uint64_t>& data)
    {
        RDPCommandType id = static_cast<RDPCommandType>((data[0] >> 56) & 0b111111);
        // Logger::Info("RDP: {}", get_rdp_command_name(id));
        switch (id)
        {
            case RDPCommandType::SyncFull:
            {
                Logger::Debug("Raising DP interrupt");
                mi_interrupt_->DP = true;
                status_.dma_busy = false;
                status_.pipe_busy = false;
                status_.start_gclk = false;
                break;
            }
            case RDPCommandType::SetColorImage:
            {
                SetColorImageCommand color_format;
                color_format.full = data[0];
                framebuffer_dram_address_ = color_format.dram_address;
                framebuffer_width_ = color_format.width + 1;
                framebuffer_format_ = color_format.format;
                // 0 = 4bpp, 1 = 8bpp, 2 = 16bpp, 3 = 32bpp
                framebuffer_pixel_size_ = 4 * (1 << color_format.size);
                break;
            }
            case RDPCommandType::Triangle:
            {
                edgewalker<false, false, false>(data);
                break;
            }
            case RDPCommandType::TriangleDepth:
            {
                edgewalker<false, false, true>(data);
                break;
            }
            case RDPCommandType::TriangleTexture:
            {
                edgewalker<false, true, false>(data);
                break;
            }
            case RDPCommandType::TriangleTextureDepth:
            {
                edgewalker<false, true, true>(data);
                break;
            }
            case RDPCommandType::TriangleShade:
            {
                edgewalker<true, false, false>(data);
                break;
            }
            case RDPCommandType::TriangleShadeDepth:
            {
                edgewalker<true, false, true>(data);
                break;
            }
            case RDPCommandType::TriangleShadeTexture:
            {
                edgewalker<true, true, false>(data);
                break;
            }
            case RDPCommandType::TriangleShadeTextureDepth:
            {
                edgewalker<true, true, true>(data);
                break;
            }
            case RDPCommandType::TextureRectangleFlip:
            case RDPCommandType::TextureRectangle:
            case RDPCommandType::Rectangle:
            {
                RectangleCommand command;
                command.full = data[0];
                // shift by 2 because these values are in 10.2 fixed point
                int32_t xmax = command.XMAX >> 2;
                int32_t ymax = command.YMAX >> 2;
                int32_t xmin = command.XMIN >> 2;
                int32_t ymin = command.YMIN >> 2;

                int z = 0;

                if (z_source_sel_)
                {
                    z = primitive_depth_;
                }

                for (int y = ymin; y < ymax; y++)
                {
                    for (int x = xmin; x < xmax; x++)
                    {
                        if (x >= scissor_xh_ && x < scissor_xl_ && y >= scissor_yh_ &&
                            y < scissor_yl_)
                        {
                            if (depth_test(x, y, z, 0))
                            {
                                draw_pixel(x, y);
                                if (z_update_en_)
                                {
                                    z_set(x, y, z);
                                }
                            }
                        }
                    }
                }
                break;
            }
            case RDPCommandType::SetFillColor:
            {
                fill_color_32_ = data[0] & 0xFFFFFFFF;
                fill_color_16_0_ = data[0] & 0xFFFF;
                fill_color_16_1_ = (data[0] >> 16);
                fill_color_32_ = hydra::bswap32(fill_color_32_);
                break;
            }
            case RDPCommandType::LoadTile:
            {
                // Loads a tile (part of the bigger texture set by SetTextureImage) into TMEM
                LoadTileCommand command;
                command.full = data[0];
                TileDescriptor& tile = tiles_[command.Tile];

                int sl = command.SL;
                int tl = command.TL;
                int sh = command.SH + 1;
                int th = command.TH + 1;

                sh *= sizeof(uint16_t);
                sl *= sizeof(uint16_t);
                for (int t = tl; t < th; t++)
                {
                    for (int s = sl; s < sh; s++)
                    {
                        uint16_t src = *reinterpret_cast<uint16_t*>(
                            &rdram_ptr_[texture_dram_address_latch_ +
                                        (t * texture_width_latch_ + s) * 2]);
                        uint16_t* dst = reinterpret_cast<uint16_t*>(
                            &tmem_[tile.tmem_address + (t - tl) * tile.line_width + (s - sl) * 2]);
                        *dst = src;
                    }
                }
                break;
            }
            case RDPCommandType::LoadBlock:
            {
                LoadBlockCommand command;
                command.full = data[0];
                TileDescriptor& tile = tiles_[command.Tile];

                int sl = command.SL;
                int tl = command.TL << 11;
                int sh = command.SH + 1;
                int DxT = command.DxT;

                bool odd = false;
                // 16 bits
                sh *= sizeof(uint16_t);
                sl *= sizeof(uint16_t);
                for (int i = sl; i < sh; i += 8)
                {
                    uint64_t src =
                        *reinterpret_cast<uint64_t*>(&rdram_ptr_[texture_dram_address_latch_ + i]);
                    if (odd)
                    {
                        src = (src >> 32) | (src << 32);
                    }
                    uint64_t* dst = reinterpret_cast<uint64_t*>(&tmem_[tile.tmem_address + i]);
                    *dst = hydra::bswap64(src);
                    tl += DxT;
                    odd = (tl >> 11) & 1;
                }
                break;
            }
            case RDPCommandType::SetTile:
            {
                // The gsDPSetTile is used to indicate where in Tmem you want to place the image,
                // how wide each line is, and the format and size of the texture.
                SetTileCommand command;
                command.full = data[0];
                TileDescriptor& tile = tiles_[command.Tile];
                tile.tmem_address = command.TMemAddress;
                tile.format = static_cast<Format>(command.format);
                tile.size = command.size; // size of tile line in 64b word
                tile.line_width = command.Line << 3;
                // This number is used as the MS 4b of an 8b index.
                tile.palette_index = command.Palette << 4;
                break;
            }
            case RDPCommandType::SetTextureImage:
            {
                // The gsDPSetTextureImage command sets a pointer to the location of the image.
                SetTextureImageCommand command;
                command.full = data[0];
                texture_dram_address_latch_ = command.DRAMAddress;
                texture_width_latch_ = command.width + 1;
                texture_format_latch_ = command.format;
                texture_pixel_size_latch_ = command.size;
                break;
            }
            case RDPCommandType::SetOtherModes:
            {
                SetOtherModesCommand command;
                command.full = data[0];
                cycle_type_ = static_cast<CycleType>(command.cycle_type);
                z_compare_en_ = command.z_compare_en;
                z_update_en_ = command.z_update_en;
                z_source_sel_ = command.z_source_sel;
                z_mode_ = command.z_mode;
                blender_1a_0_ = command.b_m1a_0;
                blender_1b_0_ = command.b_m1b_0;
                blender_2a_0_ = command.b_m2a_0;
                blender_2b_0_ = command.b_m2b_0;
                image_read_en_ = command.image_read_en;
                break;
            }
            case RDPCommandType::SetPrimDepth:
            {
                primitive_depth_ = data[0] >> 16 & 0xFFFF;
                primitive_depth_delta_ = data[0] & 0xFFFF;
                break;
            }
            case RDPCommandType::SetZImage:
            {
                zbuffer_dram_address_ = data[0] & 0x1FFFFFF;
                break;
            }
            case RDPCommandType::SetEnvironmentColor:
            {
                environment_color_ = data[0] & 0xFFFFFFFF;
                environment_color_ = hydra::bswap32(environment_color_);

                uint8_t alpha = environment_color_ >> 24;
                environment_alpha_ = (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha;
                break;
            }
            case RDPCommandType::SetBlendColor:
            {
                blend_color_ = data[0] & 0xFFFFFFFF;
                blend_color_ = hydra::bswap32(blend_color_);
                break;
            }
            case RDPCommandType::SetPrimitiveColor:
            {
                primitive_color_ = data[0] & 0xFFFFFFFF;
                primitive_color_ = hydra::bswap32(primitive_color_);

                uint8_t alpha = primitive_color_ >> 24;
                primitive_alpha_ = (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha;
                break;
            }
            case RDPCommandType::SetScissor:
            {
                SetScissorCommand command;
                command.full = data[0];
                scissor_xh_ = command.XH >> 2;
                scissor_yh_ = command.YH >> 2;
                scissor_xl_ = command.XL >> 2;
                scissor_yl_ = command.YL >> 2;
                break;
            }
            case RDPCommandType::SetFogColor:
            {
                fog_color_ = data[0] & 0xFFFFFFFF;
                fog_color_ = hydra::bswap32(fog_color_);

                uint8_t alpha = fog_color_ >> 24;
                fog_alpha_ = (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha;
                break;
            }
            case RDPCommandType::SetCombineMode:
            {
                SetCombineModeCommand command;
                command.full = data[0];

                color_sub_a_ = color_get_sub_a(command.sub_A_RGB_1);
                color_sub_b_ = color_get_sub_b(command.sub_B_RGB_1);
                color_multiplier_ = color_get_mul(command.mul_RGB_1);
                color_adder_ = color_get_add(command.add_RGB_1);

                alpha_sub_a_ = alpha_get_sub_add(command.sub_A_Alpha_1);
                alpha_sub_b_ = alpha_get_sub_add(command.sub_B_Alpha_1);
                alpha_multiplier_ = alpha_get_mul(command.mul_Alpha_1);
                alpha_adder_ = alpha_get_sub_add(command.add_Alpha_1);
                break;
            }
            default:
                Logger::Debug("Unhandled command: {} ({:02x})", get_rdp_command_name(id),
                              static_cast<int>(id));
                break;
        }
    }

    uint32_t* RDP::color_get_sub_a(uint8_t sub_a)
    {
        switch (sub_a & 0b1111)
        {
            case 0:
                return &combined_color_;
            case 1:
                return &texel_color_0_;
            case 2:
                return &texel_color_1_;
            case 3:
                return &primitive_color_;
            case 4:
                return &shade_color_;
            case 5:
                return &environment_color_;
            case 6:
                return &color_one_;
            // TODO: Implement noise
            case 7:
                return &color_zero_;
            default:
                return &color_zero_;
        }
    }

    uint32_t* RDP::color_get_sub_b(uint8_t sub_b)
    {
        switch (sub_b & 0b1111)
        {
            case 0:
                return &combined_color_;
            case 1:
                return &texel_color_0_;
            case 2:
                return &texel_color_1_;
            case 3:
                return &primitive_color_;
            case 4:
                return &shade_color_;
            case 5:
                return &environment_color_;
            // TODO: Key center??
            case 6:
                return &color_zero_;
            // TODO: Convert K4??
            case 7:
                return &color_zero_;
            default:
                return &color_zero_;
        }
    }

    uint32_t* RDP::color_get_mul(uint8_t mul)
    {
        switch (mul & 0b11111)
        {
            case 0:
                return &combined_color_;
            case 1:
                return &texel_color_0_;
            case 2:
                return &texel_color_1_;
            case 3:
                return &primitive_color_;
            case 4:
                return &shade_color_;
            case 5:
                return &environment_color_;
            case 7:
                return &combined_alpha_;
            // TODO: rest of the colors
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 23:
            case 24:
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31:
                return &color_zero_;
            default:
                Logger::WarnOnce("Unhandled mul: {}", mul);
                return &color_zero_;
        }
    }

    uint32_t* RDP::color_get_add(uint8_t add)
    {
        switch (add & 0b111)
        {
            case 0:
                return &combined_color_;
            case 1:
                return &texel_color_0_;
            case 2:
                return &texel_color_1_;
            case 3:
                return &primitive_color_;
            case 4:
                return &shade_color_;
            case 5:
                return &environment_color_;
            case 6:
                return &color_one_;
            case 7:
                return &color_zero_;
        }
        Logger::Fatal("Unreachable!");
        return nullptr;
    }

    uint32_t* RDP::alpha_get_sub_add(uint8_t sub_a)
    {
        switch (sub_a & 0b111)
        {
            case 0:
                return &combined_alpha_;
            case 1:
                return &texel_alpha_0_;
            case 2:
                return &texel_alpha_1_;
            case 3:
                return &primitive_alpha_;
            case 4:
                return &shade_alpha_;
            case 5:
                return &environment_alpha_;
            case 6:
                return &color_one_;
            default:
                return &color_zero_;
        }
    }

    uint32_t* RDP::alpha_get_mul(uint8_t mul)
    {
        switch (mul & 0b111)
        {
            case 0:
            {
                Logger::WarnOnce("Unhandled alpha mul: LOD fraction", mul);
                return &color_one_;
            }
            case 1:
                return &texel_alpha_0_;
            case 2:
                return &texel_alpha_1_;
            case 3:
                return &primitive_alpha_;
            case 4:
                return &shade_alpha_;
            case 5:
                return &environment_alpha_;
            case 6:
            {
                Logger::WarnOnce("Unhandled alpha mul: Primitive LOD fraction", mul);
                return &color_zero_;
            }
            default:
                return &color_zero_;
        }
    }

    void RDP::draw_pixel(int x, int y)
    {
        uintptr_t address = reinterpret_cast<uintptr_t>(rdram_ptr_) + framebuffer_dram_address_ +
                            (y * framebuffer_width_ + x) * (framebuffer_pixel_size_ >> 3);
        switch (cycle_type_)
        {
            case CycleType::Cycle2:
            {
                static bool warned = false;
                if (!warned)
                {
                    Logger::Warn("This game uses Cycle2, which is not implemented yet");
                    warned = true;
                }
                [[fallthrough]];
            }
            case CycleType::Cycle1:
            {
                color_combiner();
                if (framebuffer_pixel_size_ == 16)
                {
                    uint16_t* ptr = reinterpret_cast<uint16_t*>(address);
                    framebuffer_color_ = rgba16_to_rgba32(*ptr);
                    *ptr = rgba32_to_rgba16(blender());
                }
                else
                {
                    uint32_t* ptr = reinterpret_cast<uint32_t*>(address);
                    framebuffer_color_ = *ptr;
                    *ptr = blender();
                }
                break;
            }
            case CycleType::Copy:
            {
                Logger::WarnOnce("This game uses CycleType::Copy, which is not implemented yet");
                break;
            }
            case CycleType::Fill:
            {
                if (framebuffer_pixel_size_ == 16)
                {
                    uint16_t* ptr = reinterpret_cast<uint16_t*>(address);
                    *ptr = x & 1 ? fill_color_16_0_ : fill_color_16_1_;
                }
                else
                {
                    uint32_t* ptr = reinterpret_cast<uint32_t*>(address);
                    *ptr = fill_color_32_;
                }
                break;
            }
        }
    }

    uint8_t combine(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
    {
        return (a - b) * c / 0xFF + d;
    }

    void RDP::color_combiner()
    {
        uint8_t r = combine(*color_sub_a_, *color_sub_b_, *color_multiplier_, *color_adder_);
        uint8_t g = combine(*color_sub_a_ >> 8, *color_sub_b_ >> 8, *color_multiplier_ >> 8,
                            *color_adder_ >> 8);
        uint8_t b = combine(*color_sub_a_ >> 16, *color_sub_b_ >> 16, *color_multiplier_ >> 16,
                            *color_adder_ >> 16);
        uint8_t a = combine(*alpha_sub_a_, *alpha_sub_b_, *alpha_multiplier_, *alpha_adder_);
        combined_color_ = (a << 24) | (b << 16) | (g << 8) | r;
        combined_alpha_ = a << 24 | a << 16 | a << 8 | a;
    }

    uint32_t RDP::blender()
    {
        uint32_t color1, color2;
        uint8_t multiplier1, multiplier2;

        switch (blender_1a_0_ & 0b11)
        {
            case 0:
                color1 = combined_color_;
                break;
            case 1:
                color1 = framebuffer_color_;
                break;
            case 2:
                color1 = blend_color_;
                break;
            case 3:
                color1 = fog_color_;
                break;
        }

        switch (blender_2a_0_ & 0b11)
        {
            case 0:
                color2 = combined_color_;
                break;
            case 1:
                color2 = framebuffer_color_;
                break;
            case 2:
                color2 = blend_color_;
                break;
            case 3:
                color2 = fog_color_;
                break;
        }

        switch (blender_1b_0_ & 0b11)
        {
            case 0:
                multiplier1 = combined_alpha_;
                break;
            case 1:
                multiplier1 = fog_alpha_;
                break;
            case 2:
                multiplier1 = shade_alpha_;
                break;
            case 3:
                multiplier1 = 0x00;
                break;
        }

        switch (blender_2b_0_ & 0b11)
        {
            case 0:
                multiplier2 = ~multiplier1;
                break;
            case 1:
                // Apparently this is coverage
                // The bits in the framebuffer that are normally used for alpha are actually
                // coverage use 0 for now
                multiplier2 = 0x00;
                break;
            case 2:
                multiplier2 = 0xFF;
                break;
            case 3:
                multiplier2 = 0x00;
                break;
        }

        if (multiplier1 + multiplier2 == 0)
        {
            Logger::WarnOnce("Blender division by zero - blender settings: {} {} {} {}",
                             blender_1a_0_, blender_2a_0_, blender_1b_0_, blender_2b_0_);
            multiplier1 = 0xFF;
        }

        uint8_t r = (((color1 >> 0) & 0xFF) * multiplier1 + ((color2 >> 0) & 0xFF) * multiplier2) /
                    (multiplier1 + multiplier2);
        uint8_t g = (((color1 >> 8) & 0xFF) * multiplier1 + ((color2 >> 8) & 0xFF) * multiplier2) /
                    (multiplier1 + multiplier2);
        uint8_t b =
            (((color1 >> 16) & 0xFF) * multiplier1 + ((color2 >> 16) & 0xFF) * multiplier2) /
            (multiplier1 + multiplier2);

        return (0xFF << 24) | (b << 16) | (g << 8) | r;
    }

    bool RDP::depth_test(int x, int y, uint32_t z, uint16_t dz)
    {
        enum DepthMode { Opaque, Interpenetrating, Transparent, Decal };

        if (z_compare_en_)
        {
            uint32_t old_depth = z_get(x, y);
            uint16_t old_dz = dz_get(x, y);

            bool pass = false;
            switch (z_mode_ & 0b11)
            {
                // TODO: other depth modes
                case Opaque:
                {
                    // TODO: there's also some coverage stuff going on here normally
                    pass = (old_depth == 0x3ffff) || (z < old_depth);
                    break;
                }
                case Interpenetrating:
                {
                    Logger::WarnOnce("Interpenetrating depth mode not implemented");
                    pass = (old_depth == 0x3ffff) || (z < old_depth);
                    break;
                }
                case Transparent:
                {
                    pass = (old_depth == 0x3ffff) || (z < old_depth);
                    break;
                }
                case Decal:
                {
                    pass = hydra::abs(z - old_depth) <= hydra::max(dz, old_dz);
                    break;
                }
            }
            // TODO: std::unreachable
            return pass;
        }
        else
        {
            return true;
        }
    }

    uint32_t RDP::z_get(int x, int y)
    {
        uintptr_t address = reinterpret_cast<uintptr_t>(rdram_ptr_) + zbuffer_dram_address_ +
                            (y * framebuffer_width_ + x) * 2;
        uint16_t* ptr = reinterpret_cast<uint16_t*>(address);
        uint32_t decompressed = z_decompress_lut_[(*ptr >> 2) & 0x3FFF];
        return decompressed;
    }

    uint8_t RDP::dz_get(int x, int y)
    {
        uintptr_t address = reinterpret_cast<uintptr_t>(rdram_ptr_) + zbuffer_dram_address_ +
                            (y * framebuffer_width_ + x) * 2;
        uint16_t* ptr = reinterpret_cast<uint16_t*>(address);
        return *ptr & 0b11;
    }

    void RDP::z_set(int x, int y, uint32_t z)
    {
        uintptr_t address = reinterpret_cast<uintptr_t>(rdram_ptr_) + zbuffer_dram_address_ +
                            (y * framebuffer_width_ + x) * 2;
        uint16_t* ptr = reinterpret_cast<uint16_t*>(address);
        uint16_t compressed = z_compress_lut_[z & 0x3FFFF];
        *ptr = compressed;
    }

    constexpr std::array<uint8_t, 8> z_shifts = {6, 5, 4, 3, 2, 1, 0, 0};

    uint32_t RDP::z_compress(uint32_t z)
    {
        // count the most significant set bits and that is the exponent
        uint32_t exponent = std::countl_one(
            // mask bits so that we only count up to 7 bits
            (z & 0b111111100000000000)
            // shift them to the start for countl_one
            << 14);
        uint32_t mantissa = (z >> z_shifts[exponent]) & 0b111'1111'1111;
        return (exponent << 11) | mantissa;
    }

    uint32_t RDP::z_decompress(uint32_t z)
    {
        uint32_t exponent = (z >> 11) & 0x7;
        uint32_t mantissa = z & 0x7FF;
        // shift mantissa to the msb, shift back by the exponent which
        // will create n bits where n = exponent, then move those bits
        // to the correct position
        uint32_t bits = (!!exponent << 31) >> exponent;
        bits >>= 13;
        bits |= mantissa << z_shifts[exponent];
        return bits & 0x3FFFF;
    }

    void RDP::fetch_texels(int tile, int32_t s, int32_t t)
    {
        TileDescriptor& td = tiles_[tile];
        uint32_t address = td.tmem_address;
        // s ^= td.line_width ? (((t + (s * 2) / td.line_width) & 0x1) << 1) : 0;
        uint8_t byte1 = tmem_[(address + (t * td.line_width) + s * 2) & 0x1FFF];
        uint8_t byte2 = tmem_[(address + (t * td.line_width) + (s * 2) + 1) & 0x1FFF];
        texel_color_0_ = rgba16_to_rgba32((byte2 << 8) | byte1);
        texel_alpha_0_ = texel_color_0_ >> 24;
    }

    void RDP::init_depth_luts()
    {
        for (int i = 0; i < 0x4000; i++)
        {
            z_decompress_lut_[i] = z_decompress(i);
        }

        for (int i = 0; i < 0x40000; i++)
        {
            // the 2 lower bits along with 2 more from the rdrams 9th bit
            // are used to store the depth delta
            z_compress_lut_[i] = z_compress(i) << 2;
        }
    }

    template <bool Shade, bool Texture, bool Depth>
    void RDP::edgewalker(const std::vector<uint64_t>& data)
    {
        EdgeCoefficientsCommand command;
        EdgeCoefficients edgel, edgem, edgeh;
        command.full = data[0];
        edgel.full = data[1];
        edgeh.full = data[2];
        edgem.full = data[3];

        int next_block = 4;
        int tile = command.Tile;

        int32_t yh = static_cast<int16_t>(command.YH << 2) >> 4;
        int32_t ym = static_cast<int16_t>(command.YM << 2) >> 4;
        int32_t yl = static_cast<int16_t>(command.YL << 2) >> 4;
        int32_t slopel = edgel.slope;
        int32_t slopem = edgem.slope;
        int32_t slopeh = edgeh.slope;
        int32_t xl = edgel.X - slopel;
        int32_t xm = edgem.X;
        int32_t xh = edgeh.X;

        int ystart = yh;
        int ymiddle = ym;
        int yend = yl;

        int32_t xstart = xh;
        int32_t xend = xm;
        int32_t start_slope = slopeh;
        int32_t end_slope = slopem;

        bool right_major = command.lft;
        auto comparison = right_major ? std::function<bool(int, int)>(std::less_equal())
                                      : std::function<bool(int, int)>(std::greater_equal());
        int increment = right_major ? 1 : -1;

        int32_t r, g, b, a;
        int32_t DrDx, DgDx, DbDx, DaDx;
        int32_t DrDy [[maybe_unused]], DgDy [[maybe_unused]], DbDy [[maybe_unused]],
            DaDy [[maybe_unused]];
        int32_t DrDe, DgDe, DbDe, DaDe;

        if constexpr (Shade)
        {
            r = (((data[next_block] >> 48) & 0xFFFF) << 16) |
                ((data[next_block + 2] >> 48) & 0xFFFF);
            g = (((data[next_block] >> 32) & 0xFFFF) << 16) |
                ((data[next_block + 2] >> 32) & 0xFFFF);
            b = (((data[next_block] >> 16) & 0xFFFF) << 16) |
                ((data[next_block + 2] >> 16) & 0xFFFF);
            a = (((data[next_block] >> 0) & 0xFFFF) << 16) | ((data[next_block + 2] >> 0) & 0xFFFF);

            DrDx = (((data[next_block + 1] >> 48) & 0xFFFF) << 16) |
                   ((data[next_block + 3] >> 48) & 0xFFFF);
            DgDx = (((data[next_block + 1] >> 32) & 0xFFFF) << 16) |
                   ((data[next_block + 3] >> 32) & 0xFFFF);
            DbDx = (((data[next_block + 1] >> 16) & 0xFFFF) << 16) |
                   ((data[next_block + 3] >> 16) & 0xFFFF);
            DaDx = (((data[next_block + 1] >> 0) & 0xFFFF) << 16) |
                   ((data[next_block + 3] >> 0) & 0xFFFF);

            DrDe = (((data[next_block + 4] >> 48) & 0xFFFF) << 16) |
                   ((data[next_block + 6] >> 48) & 0xFFFF);
            DgDe = (((data[next_block + 4] >> 32) & 0xFFFF) << 16) |
                   ((data[next_block + 6] >> 32) & 0xFFFF);
            DbDe = (((data[next_block + 4] >> 16) & 0xFFFF) << 16) |
                   ((data[next_block + 6] >> 16) & 0xFFFF);
            DaDe = (((data[next_block + 4] >> 0) & 0xFFFF) << 16) |
                   ((data[next_block + 6] >> 0) & 0xFFFF);

            DrDy = (((data[next_block + 5] >> 48) & 0xFFFF) << 16) |
                   ((data[next_block + 7] >> 48) & 0xFFFF);
            DgDy = (((data[next_block + 5] >> 32) & 0xFFFF) << 16) |
                   ((data[next_block + 7] >> 32) & 0xFFFF);
            DbDy = (((data[next_block + 5] >> 16) & 0xFFFF) << 16) |
                   ((data[next_block + 7] >> 16) & 0xFFFF);
            DaDy = (((data[next_block + 5] >> 0) & 0xFFFF) << 16) |
                   ((data[next_block + 7] >> 0) & 0xFFFF);

            next_block += 8;
        }

        int32_t s = 0, t = 0, w = 0;
        int32_t DsDx, DtDx, DwDx;
        int32_t DsDy [[maybe_unused]], DtDy [[maybe_unused]], DwDy [[maybe_unused]];
        int32_t DsDe, DtDe, DwDe;

        if constexpr (Texture)
        {
            s = (((data[next_block] >> 48) & 0xFFFF) << 16) |
                ((data[next_block + 2] >> 48) & 0xFFFF);
            t = (((data[next_block] >> 32) & 0xFFFF) << 16) |
                ((data[next_block + 2] >> 32) & 0xFFFF);
            w = (((data[next_block] >> 16) & 0xFFFF) << 16) |
                ((data[next_block + 2] >> 16) & 0xFFFF);

            DsDx = (((data[next_block + 1] >> 48) & 0xFFFF) << 16) |
                   ((data[next_block + 3] >> 48) & 0xFFFF);
            DtDx = (((data[next_block + 1] >> 32) & 0xFFFF) << 16) |
                   ((data[next_block + 3] >> 32) & 0xFFFF);
            DwDx = (((data[next_block + 1] >> 16) & 0xFFFF) << 16) |
                   ((data[next_block + 3] >> 16) & 0xFFFF);

            DsDe = (((data[next_block + 4] >> 48) & 0xFFFF) << 16) |
                   ((data[next_block + 6] >> 48) & 0xFFFF);
            DtDe = (((data[next_block + 4] >> 32) & 0xFFFF) << 16) |
                   ((data[next_block + 6] >> 32) & 0xFFFF);
            DwDe = (((data[next_block + 4] >> 16) & 0xFFFF) << 16) |
                   ((data[next_block + 6] >> 16) & 0xFFFF);

            DsDy = (((data[next_block + 5] >> 48) & 0xFFFF) << 16) |
                   ((data[next_block + 7] >> 48) & 0xFFFF);
            DtDy = (((data[next_block + 5] >> 32) & 0xFFFF) << 16) |
                   ((data[next_block + 7] >> 32) & 0xFFFF);
            DwDy = (((data[next_block + 5] >> 16) & 0xFFFF) << 16) |
                   ((data[next_block + 7] >> 16) & 0xFFFF);

            next_block += 8;
        }

        int32_t z = 0, DzDx [[maybe_unused]], DzDy [[maybe_unused]], DzDe;

        if constexpr (Depth)
        {
            if (z_source_sel_)
            {
                z = primitive_depth_ << 16;
            }
            else
            {
                z = data[next_block] >> 32;
            }
            DzDx = data[next_block] & 0xFFFF'FFFF;
            DzDe = data[next_block + 1] >> 32;
            DzDy = data[next_block + 1] & 0xFFFF'FFFF;
        }

        int32_t r_line, g_line, b_line, a_line;
        int32_t z_line;
        int32_t s_line, t_line, w_line;
        for (int y = ystart; y < yend; y++)
        {
            if (y == ymiddle) [[unlikely]]
            {
                // we reached the middle point, change
                // our slope and end point
                // note that xstart is left unchanged
                xend = xl;
                end_slope = slopel;
            }

            if constexpr (Shade)
            {
                r += DrDe;
                g += DgDe;
                b += DbDe;
                a += DaDe;

                r_line = r;
                g_line = g;
                b_line = b;
                a_line = a;
            }

            if constexpr (Texture)
            {
                s += DsDe;
                t += DtDe;
                w += DwDe;

                s_line = s;
                t_line = t;
                w_line = w;
            }

            if constexpr (Depth)
            {
                z += DzDe;

                z_line = z;
            }

            xstart += start_slope;
            xend += end_slope;
            // get the integer part
            int xstart_i = xstart >> 16;
            int xend_i = xend >> 16;

            for (int x = xstart_i; comparison(x, xend_i); x += increment)
            {
                if (x >= scissor_xh_ && x < scissor_xl_ && y >= scissor_yh_ && y < scissor_yl_)
                {
                    if constexpr (Shade)
                    {
                        r_line += DrDx * increment;
                        g_line += DgDx * increment;
                        b_line += DbDx * increment;
                        a_line += DaDx * increment;
                        if (r_line < 0 || g_line < 0 || b_line < 0 || a_line < 0)
                            Logger::WarnOnce("Negative color value detected");
                        uint8_t final_r = std::max(0x00, std::min(0xFF, r_line >> 16));
                        uint8_t final_g = std::max(0x00, std::min(0xFF, g_line >> 16));
                        uint8_t final_b = std::max(0x00, std::min(0xFF, b_line >> 16));
                        uint8_t final_a = std::max(0x00, std::min(0xFF, a_line >> 16));
                        shade_color_ = (final_a << 24) | (final_b << 16) | (final_g << 8) | final_r;
                        shade_alpha_ = (final_a << 24) | (final_a << 16) | (final_a << 8) | final_a;
                    }

                    if constexpr (Depth)
                    {
                        z_line += DzDx * increment;
                        z_line = std::max(0, z_line);
                    }

                    if constexpr (Texture)
                    {
                        s_line += DsDx * increment;
                        t_line += DtDx * increment;
                        w_line += DwDx * increment;
                    }

                    uint32_t z_15_3 = 0;
                    bool pass_depth_test = true;

                    if constexpr (Depth)
                    {
                        z_15_3 = z_line >> 14;
                        pass_depth_test = depth_test(x, y, z_15_3, 0);
                    }

                    if (pass_depth_test)
                    {
                        if constexpr (Texture)
                        {
                            if ((w_line >> 15) != 0)
                            {
                                // s_line /= w_line >> 15;
                                // t_line /= w_line >> 15;
                                // s_line >>= 5;
                                // t_line >>= 5;
                            }
                            fetch_texels(tile, s_line / w_line, t_line / w_line);
                        }
                        draw_pixel(x, y);
                    }

                    if constexpr (Depth)
                    {
                        if (z_update_en_ && pass_depth_test)
                        {
                            z_set(x, y, z_15_3);
                        }
                    }
                }
            }
        }
    }
} // namespace hydra::N64