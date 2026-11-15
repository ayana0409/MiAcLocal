package config

import (
	"os"
	"strconv"
	"strings"
	"time"

	"miacstats/internal/model"
)

// Config holds runtime configuration for the MiAcStats service.
type Config struct {
	Port            string
	TursoURL        string
	TursoToken      string
	DefaultDeviceID string
	Timezone        string
	Location        *time.Location
	DemoMode        bool
	DefaultBilling  model.BillingSettings

	// SSO Authentication
	SSOEnabled      bool
	SSOIdentityURL  string
	SSOClientID     string
	SSOClientSecret string
	SSOScope        string
	SessionSecret   string
	SessionMaxAge   int
	SecureCookie    bool
}

// DefaultEvnTiers returns editable 6-tier residential electricity prices in VND/kWh.
func DefaultEvnTiers() []model.TierRate {
	return []model.TierRate{
		{Label: "Tier 1 (0-50 kWh)", UpperKWh: 50, RateVND: 1893},
		{Label: "Tier 2 (51-100 kWh)", UpperKWh: 100, RateVND: 1956},
		{Label: "Tier 3 (101-200 kWh)", UpperKWh: 200, RateVND: 2271},
		{Label: "Tier 4 (201-300 kWh)", UpperKWh: 300, RateVND: 2860},
		{Label: "Tier 5 (301-400 kWh)", UpperKWh: 400, RateVND: 3197},
		{Label: "Tier 6 (401+ kWh)", UpperKWh: 0, RateVND: 3302},
	}
}

// NormalizeTursoURL converts libsql:// URLs from Turso CLI into HTTPS endpoints.
func NormalizeTursoURL(raw string) string {
	trimmed := strings.TrimSpace(raw)
	trimmed = strings.TrimRight(trimmed, "/")
	if strings.HasPrefix(trimmed, "libsql://") {
		return "https://" + strings.TrimPrefix(trimmed, "libsql://")
	}
	return trimmed
}

// Load reads configuration from optional local .env file and OS environment variables.
func Load() Config {
	loadDotEnv(".env")

	tzName := getEnv("APP_TIMEZONE", "Asia/Ho_Chi_Minh")
	loc, err := time.LoadLocation(tzName)
	if err != nil {
		loc = time.FixedZone("UTC+7", 7*3600)
		tzName = "UTC+7"
	}

	tariffMode := strings.ToLower(strings.TrimSpace(getEnv("TARIFF_MODE", "evn")))
	if tariffMode != "flat" {
		tariffMode = "evn"
	}

	return Config{
		Port:            getEnv("PORT", "8080"),
		TursoURL:        NormalizeTursoURL(os.Getenv("TURSO_DATABASE_URL")),
		TursoToken:      strings.TrimSpace(os.Getenv("TURSO_AUTH_TOKEN")),
		DefaultDeviceID: getEnv("DEFAULT_DEVICE_ID", "miac-main"),
		Timezone:        tzName,
		Location:        loc,
		DemoMode:        getEnvBool("DEMO_MODE", false),
		DefaultBilling: model.BillingSettings{
			CoolWatts:    getEnvFloat("COOL_WATTS", 700), // Rated 700 W Non-Inverter
			FanLowWatts:  getEnvFloat("FAN_LOW_WATTS", 30),
			FanHighWatts: getEnvFloat("FAN_HIGH_WATTS", 50),
			TariffMode:   tariffMode,
			FlatRateVND:  getEnvFloat("FLAT_RATE_VND", 3000),
			VATPercent:   getEnvFloat("VAT_PERCENT", 8),
			BaseHomeKWh:  getEnvFloat("BASE_HOME_KWH", 100),
			EvnTiers:     DefaultEvnTiers(),
		},
		SSOEnabled:      getEnvBool("SSO_ENABLED", true),
		SSOIdentityURL:  strings.TrimRight(getEnv("SSO_IDENTITY_URL", "https://identity.thuandd.io.vn"), "/"),
		SSOClientID:     getEnv("SSO_CLIENT_ID", "QuickBite_Portal"),
		SSOClientSecret: getEnv("SSO_CLIENT_SECRET", ""),
		SSOScope:        getEnv("SSO_SCOPE", "openid profile email roles offline_access Identity"),
		SessionSecret:   getEnv("SESSION_SECRET", "miacstats-session-secret-change-in-prod-32b"),
		SessionMaxAge:   int(getEnvFloat("SESSION_MAX_AGE_SECONDS", 14*24*3600)),
		SecureCookie:    getEnvBool("SECURE_COOKIE", true),
	}
}

func loadDotEnv(path string) {
	data, err := os.ReadFile(path)
	if err != nil {
		return
	}
	for _, rawLine := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(rawLine)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, val, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		val = strings.TrimSpace(val)
		val = strings.Trim(val, `"'`)
		if key != "" && os.Getenv(key) == "" {
			_ = os.Setenv(key, val)
		}
	}
}

func getEnv(key, fallback string) string {
	if val := strings.TrimSpace(os.Getenv(key)); val != "" {
		return val
	}
	return fallback
}

func getEnvFloat(key string, fallback float64) float64 {
	raw := strings.TrimSpace(os.Getenv(key))
	if raw == "" {
		return fallback
	}
	v, err := strconv.ParseFloat(raw, 64)
	if err != nil {
		return fallback
	}
	return v
}

func getEnvBool(key string, fallback bool) bool {
	raw := strings.ToLower(strings.TrimSpace(os.Getenv(key)))
	switch raw {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	default:
		return fallback
	}
}
