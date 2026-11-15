package auth

import (
	"encoding/json"
	"strings"
)

// RequiredPermission is the permission claim required to access the admin portal.
const RequiredPermission = "xiaomi-nwt-airconditioner"

// HasRequiredPermission inspects JWT claims and verifies that RequiredPermission
// exists in the claims. It is resilient to:
// - Case sensitivity of keys ("permissions", "permission", "Permissions", etc.)
// - URI-based claim keys (e.g., http://.../claims/permissions)
// - Array types ([]string, []any)
// - JSON-encoded string arrays (e.g., "[\"xiaomi-nwt-airconditioner\"]")
// - Comma, space, or semicolon separated strings
// - Presence in custom scopes, roles, or any claim value in the signed JWT
func HasRequiredPermission(claims map[string]any) bool {
	if claims == nil {
		return false
	}

	target := strings.ToLower(strings.TrimSpace(RequiredPermission))

	// 1. First priority: Check known permission and authorization keys
	for k, val := range claims {
		kLower := strings.ToLower(k)
		if kLower == "permissions" || kLower == "permission" ||
			strings.HasSuffix(kLower, "/permissions") || strings.HasSuffix(kLower, "/permission") ||
			kLower == "scope" || kLower == "scopes" ||
			kLower == "role" || kLower == "roles" ||
			strings.HasSuffix(kLower, "/role") || strings.HasSuffix(kLower, "/roles") {
			if valueContainsTarget(val, target) {
				return true
			}
		}
	}

	// 2. Fallback: Search all claim values in case OpenIddict/ABP placed it in a custom claim
	for _, val := range claims {
		if valueContainsTarget(val, target) {
			return true
		}
	}

	return false
}

func valueContainsTarget(val any, target string) bool {
	if val == nil {
		return false
	}

	switch v := val.(type) {
	case string:
		clean := strings.ToLower(strings.TrimSpace(v))
		if clean == target {
			return true
		}

		// Try parsing as JSON array string
		if strings.HasPrefix(clean, "[") && strings.HasSuffix(clean, "]") {
			var arr []string
			if err := json.Unmarshal([]byte(v), &arr); err == nil {
				for _, item := range arr {
					if strings.ToLower(strings.TrimSpace(item)) == target {
						return true
					}
				}
			}
			var arrAny []any
			if err := json.Unmarshal([]byte(v), &arrAny); err == nil {
				for _, item := range arrAny {
					if s, ok := item.(string); ok {
						if strings.ToLower(strings.TrimSpace(s)) == target {
							return true
						}
					}
				}
			}
		}

		// Try splitting by comma, semicolon, or whitespace
		parts := strings.FieldsFunc(clean, func(r rune) bool {
			return r == ',' || r == ';' || r == ' '
		})
		for _, part := range parts {
			if strings.TrimSpace(part) == target {
				return true
			}
		}

	case []string:
		for _, p := range v {
			if strings.ToLower(strings.TrimSpace(p)) == target {
				return true
			}
		}

	case []any:
		for _, item := range v {
			if valueContainsTarget(item, target) {
				return true
			}
		}
	}

	return false
}
