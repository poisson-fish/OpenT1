; Native x87 operations from Ml/Code/m_trig.asm and m_matf.asm.
; Windows x64 ABI: pointer arguments only; matrix storage matches RMat3F.
; Matrix body preserves the original instruction and rounding order.
.const
native_two_pi REAL4 6.283185307
.code
opent1_x87_control PROC
    mov word ptr [rsp+8],cx
    fnclex
    fldcw word ptr [rsp+8]
    ret
opent1_x87_control ENDP
opent1_x87_sincosf PROC
    fld dword ptr [rcx]
    fsincos
    fstp dword ptr [r8]
    fstp dword ptr [rdx]
    ret
opent1_x87_sincosf ENDP
opent1_x87_sincosd PROC
    fld qword ptr [rcx]
    fsincos
    fstp qword ptr [r8]
    fstp qword ptr [rdx]
    ret
opent1_x87_sincosd ENDP
opent1_x87_reducef PROC
    fld dword ptr [native_two_pi]
    fld dword ptr [rcx]
    fprem
    fstp dword ptr [rdx]
    fstp st(0)
    ret
opent1_x87_reducef ENDP
opent1_x87_reduced PROC
    fld dword ptr [native_two_pi]
    fld qword ptr [rcx]
    fprem
    fstp qword ptr [rdx]
    fstp st(0)
    ret
opent1_x87_reduced ENDP
opent1_x87_matrix PROC
        fld     dword ptr [rcx+0]
        ftst
        fstsw   ax
        sahf
        fld     dword ptr [rcx+4]
        jnz     matrix_3axis

        ftst
        fstsw   ax
        sahf
        fld     dword ptr [rcx+8]
        jnz     matrix_2axis

        ; get rid of x,y
        ffree   st(2)
        ffree   st(1)

        ftst
        fstsw   ax
        sahf
        jnz     matrix_1axis
matrix_0axis:
   ; the matrix looks like this:
   ;  1        0        0
   ;  0        1        0
   ;  0        0        1
   ;
   ; num_axis = 0
        ; get rid of z, get 1.0
        ffree   st(0)
        fld1

        sub     eax,eax

        fst     dword ptr [rdx+4]
        mov     dword ptr [rdx+8],eax
        mov     dword ptr [rdx+16],eax
        fst     dword ptr [rdx+20]
        mov     dword ptr [rdx+12],eax
        mov     dword ptr [rdx+24],eax
        mov     dword ptr [rdx+28],eax
        mov     dword ptr [rdx+32],eax
        fstp    dword ptr [rdx+36]

	mov	eax,dword ptr [rdx]
	and	eax,NOT (4 OR 1)
	mov	dword ptr [rdx],eax

        jmp     matrix_done

matrix_1axis:

   ; the matrix looks like this:
   ;  cos(z)   sin(z)   0
   ;  -sin(z)  cos(z)   0
   ;  0        0        1
   ;
   ; num_axis = 1
        fsincos
        sub     eax,eax
        mov     dword ptr [rdx+12],eax
        mov     dword ptr [rdx+24],eax
        mov     dword ptr [rdx+28],eax
        mov     dword ptr [rdx+32],eax
        mov     eax,03f800000h
        mov     dword ptr [rdx+36],eax
        fst     dword ptr [rdx+4]     ; cos(z)
        fxch
        fst     dword ptr [rdx+8]     ; sin(z)
        fchs
        fxch
        fstp    dword ptr [rdx+20]    ; cos(z)
        fstp    dword ptr [rdx+16]    ; -sin(z)

	mov	eax,dword ptr [rdx]
	or	eax,1
	and	eax,NOT (4)
	mov	dword ptr [rdx],eax

        jmp     matrix_done

matrix_3axis:
        fld     dword ptr [rcx+8]
matrix_2axis:

   ; the matrix looks like this:
   ;  r1 - (r4 * sin(x))     r2 + (r3 * sin(x))   -cos(x) * sin(y)
   ;  -cos(x) * sin(z)       cos(x) * cos(z)      sin(x)
   ;  r3 + (r2 * sin(x))     r4 - (r1 * sin(x))   cos(x) * cos(y)
   ;
   ; where:
   ;  r1 = cos(y) * cos(z)
   ;  r2 = cos(y) * sin(z)
   ;  r3 = sin(y) * cos(z)
   ;  r4 = sin(y) * sin(z)
   ;
   ; num_axis = 3

        fsincos                                 ; cz, sz, y, x
        fxch    st(2)                           ; y, sz, cz, x
        fsincos                                 ; cy, sy, sz, cz, x
        fxch    st(4)                           ; x, sy, sz, cz, cy
        fsincos                                 ; cx, sx, sy, sz, cz, cy

        fld     st(5)                           ; cy, ...
        fmul    st,st(1)                        ; cx*cy, cx, sx, sy, sz, ...

        fld     st(5)                           ; cz, m3_2_2, cx, ...
        fmul    st,st(2)                        ; cx*cz, m3_2_2, ...
        fxch    st(1)                           ; m3_2_2, m3_1_1, ...
        fstp    dword ptr [rdx+36]
        fstp    dword ptr [rdx+20]

        fchs                                    ; -cx, sx, sy, sz, cz, cy
        fld     st(2)                           ; sy, -cx, ...
        fmul    st, st(1)                       ; -cx*sy, ...
        fxch    st(1)                           ; -cx, m3_0_2, sx, sy, sz, ...

        fmul    st, st(4)                       ; -cx*sz, m3_0_2, sx, sy, ...
        fxch    st(1)                           ; m3_0_2, m3_1_0, sx, sy, ...
        fstp    dword ptr [rdx+12]        ; m3_1_0, sx, sy, sz, ...
        fstp    dword ptr [rdx+16]        ; sx, sy, sz, cz, cy

        fxch    st(4)                           ; cy, sy, sz, cz, sx

        fld     st(0)                           ; cy, cy, ...
        fmul    st, st(3)                       ; cy*sz, cy, sy, sz, cz, sx
        fxch    st(1)                           ; cy, cy*sz, sy, sz, cz, sx
        fmul    st, st(4)                       ; cy*cz, cy*sz, sy, sz, cz, sx
        fxch    st(2)                           ; sy, cy*sz, cy*cz, sz, cz, sx
        fmul    st(3), st
        fmulp   st(4), st                       ; cy*sz, cy*cz, sy*sz, sy*cz, sx

        fxch    st(4)                           ; sx, cy*cz, sy*sz, sy*cz, cy*sz
        fst     dword ptr [rdx+24]        ; sx, cy*cz, sy*sz, sy*cz, cy*sz
        fld     st(2)                           ; sy*sz, sx, cy*cz, sy*sz, sy*cz, cy*sz
        fld     st(2)                           ; cy*cz, sy*sz, sx, cy*cz, sy*sz, sy*cz, cy*sz
        fld     st(2)                           ; sx, cy*cz, sy*sz, sx, cy*cz, sy*sz, ...

        fmulp   st(1),st                        ; sx*cy*cz, sy*sz, sx, cy*cz, sy*sz, ...
        fld     st(2)                           ; sx, sx*cy*cz, sy*sz, sx, cy*cz, sy*sz, ...
        fxch    st(3)                           ; sx, sx*cy*cz, sy*sz, sx, cy*cz, sy*sz, ...
        fmulp   st(2),st                        ; sx*cy*cz, sx*sy*sz, sx, cy*cz, sy*sz, ...
        fsubp   st(4),st                        ; sx*sy*sz, sx, cy*cz, m3_2_1, ...
        fsubp   st(2),st                        ; sx, m3_0_0, m3_2_1, sy*cz, cy*sz

        fxch    st(2)                           ; m3_2_1, m3_0_0, sx, sy*cz, cy*sz
        fstp    dword ptr [rdx+32]        ; m3_0_0, sx, sy*cz, cy*sz
        fstp    dword ptr [rdx+4]        ; sx, sy*cz, cy*sz

        fld     st(2)                           ; cy*sz, sx, sy*cz, cy*sz
        fld     st(2)                           ; sy*cz, cy*sz, sx, sy*cz, cy*sz
        fld     st(2)                           ; sx, sy*cz, cy*sz, sx, sy*cz, cy*sz
        fxch    st(1)                           ; sy*cz, sx, cy*sz, sx, sy*cz, cy*sz
        fmulp   st(3),st                        ; sx, cy*sz, sx*sy*cz, sy*cz, cy*sz
        fmulp   st(1),st                        ; sx*cy*sz, sx*sy*cz, sy*cz, cy*sz
        fxch    st(1)                           ; sx*sy*cz, sx*cy*sz, sy*cz, cy*sz
        faddp   st(3),st                        ; sx*cy*sz, sy*cz, m3_0_1
        faddp   st(1),st                        ; m3_2_0, m3_0_1

        fxch    st(1)                           ; m3_0_1, m3_2_0
        fstp    dword ptr [rdx+8]        ; m3_2_0
        fstp    dword ptr [rdx+28]

	mov	eax,dword ptr [rdx]
	or	eax,1
	and	eax, NOT (4)
	mov	dword ptr [rdx],eax

        jmp     matrix_done

matrix_done:
        RET
opent1_x87_matrix ENDP
END
