package web

import (
	"embed"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"sync"
	"time"

	"miacstats/internal/config"
	"miacstats/internal/model"
	"miacstats/internal/stats"
	"miacstats/internal/turso"
)

//go:embed templates/index.html
var embeddedAssets embed.FS

// Server handles HTTP endpoints for MiAcStats dashboard and JSON APIs.
type Server struct {
	cfg         config.Config
	store       turso.Store
	indexHTML   []byte
	mu          sync.RWMutex
	memSettings model.BillingSettings
	nowFn       func() time.Time
}

// NewServer constructs an HTTP handler for the MiAcStats service.
func NewServer(cfg config.Config, store turso.Store) (http.Handler, error) {
	htmlBytes, err := embeddedAssets.ReadFile("templates/index.html")
	if err != nil {
		return nil, err
	}
	s := &Server{
		cfg:         cfg,
		store:       store,
		indexHTML:   htmlBytes,
		memSettings: cfg.DefaultBilling,
		nowFn:       time.Now,
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.handleHealth)
	mux.HandleFunc("GET /", s.handleIndex)
	mux.HandleFunc("GET /api/stats", s.handleGetStats)
	mux.HandleFunc("POST /api/settings", s.handlePostSettings)
	return mux, nil
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":        true,
		"demo_mode": s.isDemoActive(),
	})
}

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(s.indexHTML)
}

func (s *Server) handleGetStats(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	deviceID := strings.TrimSpace(q.Get("device"))
	if deviceID == "" {
		deviceID = s.cfg.DefaultDeviceID
	}

	nowLocal := s.nowFn().In(s.cfg.Location)

	// Default: 1st of current calendar month 00:00 to end of current month 23:59
	startOfMonth := time.Date(nowLocal.Year(), nowLocal.Month(), 1, 0, 0, 0, 0, s.cfg.Location)
	endOfMonth := startOfMonth.AddDate(0, 1, -1)
	endOfMonth = time.Date(endOfMonth.Year(), endOfMonth.Month(), endOfMonth.Day(), 23, 59, 59, 0, s.cfg.Location)

	fromRaw := strings.TrimSpace(q.Get("from"))
	toRaw := strings.TrimSpace(q.Get("to"))

	fromTime := startOfMonth
	if fromRaw != "" {
		parsed, err := parseDateTime(fromRaw, s.cfg.Location, false)
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, "invalid 'from' date/time (use YYYY-MM-DD or YYYY-MM-DDTHH:mm)")
			return
		}
		fromTime = parsed
	}

	toTime := endOfMonth
	if toRaw != "" {
		parsed, err := parseDateTime(toRaw, s.cfg.Location, true)
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, "invalid 'to' date/time (use YYYY-MM-DD or YYYY-MM-DDTHH:mm)")
			return
		}
		toTime = parsed
	}

	if toTime.Before(fromTime) {
		writeJSONError(w, http.StatusBadRequest, "'to' date/time must not precede 'from' date/time")
		return
	}
	if toTime.Sub(fromTime) > 366*24*time.Hour {
		writeJSONError(w, http.StatusBadRequest, "date range cannot exceed 366 days")
		return
	}

	fromUTC := fromTime.UTC().Unix()
	toUTC := toTime.UTC().Unix()

	billing := s.currentDefaults()
	var buckets []model.Bucket

	if s.store != nil {
		loadedCfg, err := s.store.LoadBillingSettings(r.Context(), billing)
		if err == nil {
			billing = stats.NormalizeSettings(loadedCfg, s.cfg.DefaultBilling)
		}
		buckets, err = s.store.QueryBuckets(r.Context(), deviceID, fromUTC, toUTC)
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, "failed to query Turso: "+err.Error())
			return
		}
	} else if s.cfg.DemoMode {
		allDemo := stats.DemoBuckets(s.nowFn(), s.cfg.Location, deviceID)
		for _, b := range allDemo {
			if b.TsUTC >= fromUTC && b.TsUTC <= toUTC {
				buckets = append(buckets, b)
			}
		}
	}

	dash := stats.ComputeDashboard(
		deviceID,
		fromTime.Format("2006-01-02T15:04"),
		toTime.Format("2006-01-02T15:04"),
		s.cfg.Location,
		s.isDemoActive(),
		buckets,
		billing,
	)
	writeJSON(w, http.StatusOK, dash)
}

func (s *Server) handlePostSettings(w http.ResponseWriter, r *http.Request) {
	var in model.BillingSettings
	if err := decodeStrictJSON(r, &in); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	if in.CoolWatts <= 0 || in.FanLowWatts < 0 || in.FanHighWatts < 0 {
		writeJSONError(w, http.StatusBadRequest, "cool_watts must be positive, fan watts must be non-negative")
		return
	}
	if in.TariffMode != "evn" && in.TariffMode != "flat" {
		writeJSONError(w, http.StatusBadRequest, "tariff_mode must be 'evn' or 'flat'")
		return
	}
	if in.VATPercent < 0 || in.VATPercent > 100 || in.FlatRateVND < 0 || in.BaseHomeKWh < 0 {
		writeJSONError(w, http.StatusBadRequest, "invalid rate, base kWh, or VAT percentage")
		return
	}

	cleaned := stats.NormalizeSettings(in, s.cfg.DefaultBilling)
	if s.store != nil {
		if err := s.store.SaveBillingSettings(r.Context(), cleaned); err != nil {
			writeJSONError(w, http.StatusBadGateway, "failed to save settings to Turso: "+err.Error())
			return
		}
	}

	s.mu.Lock()
	s.memSettings = cleaned
	s.mu.Unlock()

	writeJSON(w, http.StatusOK, map[string]any{
		"ok":       true,
		"settings": cleaned,
	})
}

func (s *Server) currentDefaults() model.BillingSettings {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.memSettings
}

func (s *Server) isDemoActive() bool {
	return s.store == nil && s.cfg.DemoMode
}

func parseDateTime(raw string, loc *time.Location, isEnd bool) (time.Time, error) {
	raw = strings.TrimSpace(raw)
	formats := []string{
		"2006-01-02T15:04:05",
		"2006-01-02T15:04",
		"2006-01-02 15:04",
		"2006-01-02",
	}
	for _, fmtStr := range formats {
		if t, err := time.ParseInLocation(fmtStr, raw, loc); err == nil {
			if fmtStr == "2006-01-02" && isEnd {
				return time.Date(t.Year(), t.Month(), t.Day(), 23, 59, 59, 0, loc), nil
			}
			return t, nil
		}
	}
	return time.Time{}, errors.New("cannot parse datetime")
}

func decodeStrictJSON(r *http.Request, dst any) error {
	r.Body = http.MaxBytesReader(nil, r.Body, 1<<20)
	dec := json.NewDecoder(r.Body)
	if err := dec.Decode(dst); err != nil {
		return errors.New("invalid JSON payload")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeJSONError(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]any{
		"ok":    false,
		"error": msg,
	})
}
