package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"os"

	"github.com/icza/screp/rep/repcmd"
	"github.com/icza/screp/repparser"
)

func run(mode, path string) error {
	if mode != "info" && mode != "commands" && mode != "extract-map" {
		return fmt.Errorf("unknown operation %q", mode)
	}
	r, err := repparser.ParseFileConfig(path, repparser.Config{
		Commands: mode != "extract-map",
		MapData:  mode != "commands",
		Debug:    mode == "extract-map",
		Logger:   log.New(io.Discard, "", 0),
	})
	if err != nil {
		return err
	}
	encoder := json.NewEncoder(os.Stdout)
	switch mode {
	case "extract-map":
		if r.MapData == nil || r.MapData.Debug == nil || len(r.MapData.Debug.Data) == 0 {
			return fmt.Errorf("replay has no embedded CHK data")
		}
		_, err = os.Stdout.Write(r.MapData.Debug.Data)
		return err
	case "commands":
		for _, command := range r.Commands.Cmds {
			base := command.BaseCmd()
			production := false
			switch command.(type) {
			case *repcmd.BuildCmd, *repcmd.TrainCmd, *repcmd.BuildingMorphCmd,
				*repcmd.TechCmd, *repcmd.UpgradeCmd:
				production = true
			}
			err := encoder.Encode(map[string]any{
				"frame": base.Frame, "time_ms": base.Frame.Duration().Milliseconds(),
				"player_id": base.PlayerID, "type": base.Type.String(),
				"production": production, "params": command.Params(true), "details": command,
			})
			if err != nil {
				return err
			}
		}
		for _, command := range r.Commands.ParseErrCmds {
			base := command.BaseCmd()
			err := encoder.Encode(map[string]any{
				"frame": base.Frame, "time_ms": base.Frame.Duration().Milliseconds(),
				"player_id": base.PlayerID, "type": base.Type.String(),
				"production": false, "parse_error": true, "details": command,
			})
			if err != nil {
				return err
			}
		}
		return nil
	default:
		r.Compute()
		exits := make([]map[string]any, 0, len(r.Computed.LeaveGameCmds))
		for _, leave := range r.Computed.LeaveGameCmds {
			event := map[string]any{
				"player_id": leave.PlayerID, "frame": leave.Frame,
				"time_ms": leave.Frame.Duration().Milliseconds(), "reason": leave.Reason.String(),
			}
			if player := r.Header.PIDPlayers[leave.PlayerID]; player != nil {
				event["slot"] = player.SlotID
			}
			exits = append(exits, event)
		}
		var mapData map[string]any
		if m := r.MapData; m != nil {
			mapData = map[string]any{
				"name": m.Name, "description": m.Description, "version": m.Version,
				"tileset": m.TileSet, "start_locations": m.StartLocations,
				"mineral_fields": len(m.MineralFields), "geysers": len(m.Geysers),
			}
		}
		return encoder.Encode(map[string]any{
			"header": r.Header, "duration_ms": r.Header.Duration().Milliseconds(),
			"map": mapData, "command_count": len(r.Commands.Cmds),
			"command_parse_errors": len(r.Commands.ParseErrCmds),
			"player_stats":         r.Computed.PlayerDescs, "leave_events": exits,
			"winner_team_estimate": r.Computed.WinnerTeam,
		})
	}
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "Usage: stl-rep-parser <info|commands|extract-map> <replay.rep>")
		os.Exit(2)
	}
	if err := run(os.Args[1], os.Args[2]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
