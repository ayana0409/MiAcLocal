package auth

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// TokenResponse represents the OAuth2 token endpoint response from QuickBite Identity.
type TokenResponse struct {
	AccessToken      string `json:"access_token"`
	TokenType        string `json:"token_type"`
	ExpiresIn        int64  `json:"expires_in"`
	RefreshToken     string `json:"refresh_token"`
	IDToken          string `json:"id_token"`
	Scope            string `json:"scope"`
	Error            string `json:"error"`
	ErrorDescription string `json:"error_description"`
}

// IdentityClient executes token requests against QuickBite Identity Server.
type IdentityClient struct {
	baseURL      string
	clientID     string
	clientSecret string
	scope        string
	httpClient   *http.Client
}

// NewIdentityClient creates a new OAuth client for QuickBite Identity.
func NewIdentityClient(baseURL, clientID, clientSecret, scope string, client *http.Client) *IdentityClient {
	if client == nil {
		client = &http.Client{Timeout: 15 * time.Second}
	}
	cleanBase := strings.TrimRight(strings.TrimSpace(baseURL), "/")
	return &IdentityClient{
		baseURL:      cleanBase,
		clientID:     clientID,
		clientSecret: strings.TrimSpace(clientSecret),
		scope:        scope,
		httpClient:   client,
	}
}

// LoginPassword exchanges username and password for tokens (Password Grant).
func (c *IdentityClient) LoginPassword(ctx context.Context, username, password string) (*TokenResponse, error) {
	data := url.Values{}
	data.Set("grant_type", "password")
	data.Set("client_id", c.clientID)
	if c.clientSecret != "" {
		data.Set("client_secret", c.clientSecret)
	}
	data.Set("scope", c.scope)
	data.Set("username", username)
	data.Set("password", password)

	return c.postToken(ctx, data)
}

// RefreshToken exchanges a refresh token for new access and refresh tokens.
func (c *IdentityClient) RefreshToken(ctx context.Context, refreshToken string) (*TokenResponse, error) {
	if refreshToken == "" {
		return nil, errors.New("refresh token is required")
	}

	data := url.Values{}
	data.Set("grant_type", "refresh_token")
	data.Set("client_id", c.clientID)
	if c.clientSecret != "" {
		data.Set("client_secret", c.clientSecret)
	}
	data.Set("scope", c.scope)
	data.Set("refresh_token", refreshToken)

	return c.postToken(ctx, data)
}

func (c *IdentityClient) postToken(ctx context.Context, data url.Values) (*TokenResponse, error) {
	tokenEndpoint := c.baseURL + "/connect/token"

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, tokenEndpoint, strings.NewReader(data.Encode()))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("identity request failed: %w", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, fmt.Errorf("read response body: %w", err)
	}

	var tr TokenResponse
	if err := json.Unmarshal(body, &tr); err != nil {
		return nil, fmt.Errorf("decode token response: %w", err)
	}

	if resp.StatusCode != http.StatusOK || tr.Error != "" {
		errMsg := tr.Error
		if tr.ErrorDescription != "" {
			errMsg += ": " + tr.ErrorDescription
		}
		if errMsg == "" {
			errMsg = fmt.Sprintf("HTTP %d", resp.StatusCode)
		}
		return nil, errors.New(errMsg)
	}

	if tr.AccessToken == "" {
		return nil, errors.New("missing access_token in response")
	}

	return &tr, nil
}

