# Relatório de Ajustes - Dependências do Camera Pipeline

## Alterações Realizadas

1. **Simplificação da estrutura de dependências**:
   - NumPy foi movido de dependência opcional para dependência obrigatória (versão >=1.26.4)
   - Removida a opção `[numpy]` para instalação com NumPy
   - Mantida apenas a opção `[dev]` para instalação de desenvolvimento

2. **Alterações nos arquivos de configuração**:
   - Atualizado `pyproject.toml` para incluir NumPy nas dependências obrigatórias
   - Criado `README.md` com instruções simplificadas para instalação

3. **Alterações no código**:
   - Removidas verificações condicionais para a presença de NumPy em `processor.py`
   - Removida a lógica alternativa para processamento de frames sem NumPy
   - Atualizado `callbacks.py` para assumir NumPy como dependência obrigatória
   - Mantida a propriedade `has_numpy` por compatibilidade, agora sempre retornando `True`

## Estrutura de Dependências Atual

### Dependências Obrigatórias
- Python >= 3.10
- opencv-python==4.11.0.86
- numpy>=1.26.4

### Dependências Opcionais
- `[dev]`: pytest, black, flake8 (para desenvolvimento)
- `[test]`: pytest (para testes)

## Instruções de Instalação

### Instalação Básica
```bash
pip install git+https://github.com/leandroZanatta/camera-pipeline.git
```

### Instalação para Desenvolvimento
```bash
git clone https://github.com/leandroZanatta/camera-pipeline.git
cd camera-pipeline
pip install -e ".[dev]"
```

## Verificação

A verificação da instalação confirmou que as alterações foram aplicadas corretamente:
- NumPy está sendo instalado automaticamente como dependência obrigatória
- O código não precisa mais verificar se NumPy está disponível
- A estrutura de instalação foi simplificada para um único comando básico 