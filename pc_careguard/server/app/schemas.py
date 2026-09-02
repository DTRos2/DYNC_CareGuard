from pydantic import BaseModel


class LoginRequest(BaseModel):
    login_id: str
    password: str


class LoginResponse(BaseModel):
    id: int
    login_id: str
    role: str
