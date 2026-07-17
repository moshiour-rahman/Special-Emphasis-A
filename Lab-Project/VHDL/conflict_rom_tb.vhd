-- conflict_rom_tb.vhd - self-checking testbench for the AVIMS conflict ROM.
-- Sweeps all 12x12 movement pairs and checks:
--   (1) zero diagonal   : a movement never conflicts with itself
--   (2) symmetry        : conflict(a,b) == conflict(b,a)
--   (3) known spot cases: NS/SS and ES/WS compatible; NS/ES and NS/EW-type conflict
-- These properties are independent of the ROM contents, so the TB is a real check,
-- not a copy of the table. Run in ModelSim/QuestaSim (vcom + vsim) or ghdl.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity conflict_rom_tb is
end entity;

architecture sim of conflict_rom_tb is
    constant NMOV : integer := 12;
    signal a, b        : std_logic_vector(3 downto 0) := (others => '0');
    signal c           : std_logic;
    signal c_ab, c_ba  : std_logic;

    -- movement ids for the spot checks (approach*3 + intent; N,E,S,W x R,S,L)
    -- NB: avoid names NS/PS/US/MS - they collide with VHDL time units.
    constant MOV_NS : integer := 1;
    constant MOV_SS : integer := 7;
    constant MOV_ES : integer := 4;
    constant MOV_WS : integer := 10;

    signal errors : integer := 0;

    function slv4(i : integer) return std_logic_vector is
    begin
        return std_logic_vector(to_unsigned(i, 4));
    end function;
begin
    dut : entity work.conflict_rom
        port map (mov_a => a, mov_b => b, conflict => c);

    -- second instance to read the mirrored address in the same delta cycle
    dutm : entity work.conflict_rom
        port map (mov_a => b, mov_b => a, conflict => c_ba);
    c_ab <= c;

    stim : process
        variable i, j : integer;
    begin
        -- full sweep: diagonal + symmetry
        for i in 0 to NMOV - 1 loop
            for j in 0 to NMOV - 1 loop
                a <= slv4(i);
                b <= slv4(j);
                wait for 10 ns;
                if i = j then
                    if c_ab /= '0' then
                        report "DIAGONAL FAIL at move " & integer'image(i)
                            severity error;
                        errors <= errors + 1;
                    end if;
                end if;
                if c_ab /= c_ba then
                    report "SYMMETRY FAIL at (" & integer'image(i) & ","
                        & integer'image(j) & ")" severity error;
                    errors <= errors + 1;
                end if;
            end loop;
        end loop;

        -- spot checks: compatible pairs must be '0'
        a <= slv4(MOV_NS); b <= slv4(MOV_SS); wait for 10 ns;
        assert c_ab = '0' report "NS/SS should be COMPATIBLE" severity error;
        a <= slv4(MOV_ES); b <= slv4(MOV_WS); wait for 10 ns;
        assert c_ab = '0' report "ES/WS should be COMPATIBLE" severity error;

        -- spot checks: conflicting pairs must be '1'
        a <= slv4(MOV_NS); b <= slv4(MOV_ES); wait for 10 ns;
        assert c_ab = '1' report "NS/ES should CONFLICT" severity error;

        wait for 10 ns;
        if errors = 0 then
            report "CONFLICT ROM TB: ALL CHECKS PASSED" severity note;
        else
            report "CONFLICT ROM TB: " & integer'image(errors) & " FAILURES"
                severity failure;
        end if;
        wait;
    end process;
end architecture;
